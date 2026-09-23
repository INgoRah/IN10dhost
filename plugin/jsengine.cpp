/*
 * QuickJS scripting plugin.
 *
 * Loads the .js file named by the "script" key of its plugin config and
 * forwards every ActionEvent to a global onAction(code, val, data)
 * function in it. data is the event payload as a plain JS object, or
 * null when the action carries none.
 *
 * Scripts reach the 1-Wire devices through the global "ow" object:
 *   ow.log(msg)              write to the daemon log
 *   ow.pio_set(rom, pio)     drive a ds2408 PIO, returns true on success
 *   ow.temp(rom)             read a ds1820 temperature, null if not one
 *   ow.volt(rom, channel)    read a cached ds2450 voltage
 * rom is the dotted string form ("29.0200FDFF6677F8") that the event
 * payloads also carry - JS numbers cannot hold a 64 bit rom code
 * exactly, so the string is the only safe round trip.
 */
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "plugin.h"
#include "interface/ds2408.h"
#include "interface/ds1820.h"
#include "interface/ds2450.h"

#include <quickjs.h>

/* how long one script callback may run before it is interrupted */
#define SCRIPT_TIMEOUT_MS 200

class JsEngine : public Plugin {
private:
	ILogger* logger;
	IDevices* devices;
	JSRuntime* rt;
	JSContext* ctx;
	std::string script;
	/* QuickJS is single threaded, but action() is reached both from the
	   FUSE threads and from the background poll worker */
	std::recursive_mutex mtx;
	/* a script that drives a PIO from its ACT_DEV_CHANGE handler would
	   otherwise re-enter through ds2408::pio_set() without end */
	int depth;
	std::chrono::steady_clock::time_point deadline;

	static uint64_t rom_to_code(const char* rom)
	{
		std::string s(rom ? rom : "");
		size_t dot = s.find('.');
		if (dot != std::string::npos)
			s.erase(dot, 1);
		return std::strtoull(s.c_str(), nullptr, 16);
	}

	/* Looks up a device by the rom string in argv[0]. Returns nullptr
	   when the argument is missing or no such device exists. */
	static IDev* arg_dev(JSContext* ctx, int argc, JSValueConst* argv)
	{
		JsEngine* self = (JsEngine*)JS_GetContextOpaque(ctx);

		if (argc < 1 || self == nullptr || self->devices == nullptr)
			return nullptr;
		const char* rom = JS_ToCString(ctx, argv[0]);
		if (rom == nullptr)
			return nullptr;
		IDev* dev = self->devices->get_dev(rom_to_code(rom));
		JS_FreeCString(ctx, rom);

		return dev;
	}

	static JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		JsEngine* self = (JsEngine*)JS_GetContextOpaque(ctx);

		if (argc < 1 || self == nullptr || self->logger == nullptr)
			return JS_UNDEFINED;
		const char* msg = JS_ToCString(ctx, argv[0]);
		if (msg == nullptr)
			return JS_EXCEPTION;
		self->logger->info(std::string("js: ") + msg);
		JS_FreeCString(ctx, msg);

		return JS_UNDEFINED;
	}

	static JSValue js_pio_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		int32_t pio = 0;

		if (argc < 2)
			return JS_FALSE;
		IDS2408* d = dynamic_cast<IDS2408*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_FALSE;
		if (JS_ToInt32(ctx, &pio, argv[1]) < 0)
			return JS_EXCEPTION;

		return JS_NewBool(ctx, d->pio_set((uint8_t)pio) == 0xAA);
	}

	static JSValue js_temp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		IDS1820* d = dynamic_cast<IDS1820*>(arg_dev(ctx, argc, argv));

		if (d == nullptr)
			return JS_NULL;

		return JS_NewFloat64(ctx, d->temp_read(1));
	}

	static JSValue js_volt(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		int32_t ch = 0;

		if (argc < 2)
			return JS_NULL;
		IDS2450* d = dynamic_cast<IDS2450*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_NULL;
		if (JS_ToInt32(ctx, &ch, argv[1]) < 0)
			return JS_EXCEPTION;

		return JS_NewFloat64(ctx, d->adc_get((uint8_t)ch));
	}

	/* Runs periodically while script code executes; a non zero return
	   aborts it, which is what stops a runaway loop from hanging the
	   whole daemon. */
	static int js_interrupt(JSRuntime*, void* opaque)
	{
		JsEngine* self = (JsEngine*)opaque;

		return std::chrono::steady_clock::now() > self->deadline ? 1 : 0;
	}

	void log_exception(const char* what)
	{
		JSValue e = JS_GetException(ctx);
		const char* msg = JS_ToCString(ctx, e);

		if (logger)
			logger->error(std::format("jsengine: {} failed: {}", what, msg ? msg : "?"));
		if (msg)
			JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, e);
	}

	/* Arms the watchdog, runs fn, and reports anything it threw. */
	JSValue call_guarded(JSValue fn, JSValue self_obj, int argc, JSValue* argv, const char* what)
	{
		/* The runtime measures stack depth against the stack top of
		   whichever thread created it, but actions arrive from the
		   background poll worker and the FUSE threads as well. Without
		   re-basing it here every call from one of those threads fails
		   with "Maximum call stack size exceeded". Safe because the
		   mutex means only one thread is ever inside the engine. */
		JS_UpdateStackTop(rt);
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(SCRIPT_TIMEOUT_MS);
		depth++;
		JSValue res = JS_Call(ctx, fn, self_obj, argc, argv);
		depth--;
		if (JS_IsException(res))
			log_exception(what);

		return res;
	}

	void register_globals()
	{
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue owobj = JS_NewObject(ctx);

		JS_SetPropertyStr(ctx, owobj, "log",
			JS_NewCFunction(ctx, js_log, "log", 1));
		JS_SetPropertyStr(ctx, owobj, "pio_set",
			JS_NewCFunction(ctx, js_pio_set, "pio_set", 2));
		JS_SetPropertyStr(ctx, owobj, "temp",
			JS_NewCFunction(ctx, js_temp, "temp", 1));
		JS_SetPropertyStr(ctx, owobj, "volt",
			JS_NewCFunction(ctx, js_volt, "volt", 2));
		JS_SetPropertyStr(ctx, global, "ow", owobj);
		JS_FreeValue(ctx, global);
	}

	void load_script()
	{
		std::ifstream f(script);

		if (!f) {
			if (logger)
				logger->error(std::format("jsengine: cannot open script {}", script));
			return;
		}
		std::stringstream buf;
		buf << f.rdbuf();
		std::string src = buf.str();

		/* config_set() can also arrive on a FUSE thread, see the note
		   in call_guarded() */
		JS_UpdateStackTop(rt);
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(SCRIPT_TIMEOUT_MS);
		JSValue res = JS_Eval(ctx, src.c_str(), src.size(), script.c_str(),
			JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(res))
			log_exception(script.c_str());
		else if (logger)
			logger->info(std::format("jsengine: loaded {}", script));
		JS_FreeValue(ctx, res);
	}

public:
	JsEngine() : logger(nullptr), devices(nullptr), rt(nullptr), ctx(nullptr), depth(0) {}

	void info() override {
		std::cout << "QuickJS engine " << JS_GetVersion() << std::endl;
	}

	void init(ILogger* logger, IDevices* devices) override
	{
		this->logger = logger;
		this->devices = devices;

		rt = JS_NewRuntime();
		if (rt == nullptr) {
			logger->error("jsengine: cannot create runtime");
			return;
		}
		JS_SetInterruptHandler(rt, js_interrupt, this);
		ctx = JS_NewContext(rt);
		if (ctx == nullptr) {
			logger->error("jsengine: cannot create context");
			JS_FreeRuntime(rt);
			rt = nullptr;
			return;
		}
		JS_SetContextOpaque(ctx, this);
		register_globals();
		logger->debug("jsengine: initialized");
	}

	void exit() override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);

		if (ctx) {
			JS_FreeContext(ctx);
			ctx = nullptr;
		}
		if (rt) {
			JS_FreeRuntime(rt);
			rt = nullptr;
		}
		logger = nullptr;
		devices = nullptr;
	}

	json config_get() const override
	{
		json j;
		j["script"] = script;

		return j;
	}

	void config_set(const json& j) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);

		script = j.value("script", "");
		if (ctx && !script.empty())
			load_script();
	}

	int action(const ActionEvent& ev) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		int32_t ret = 0;

		if (ctx == nullptr)
			return 0;
		if (depth > 0) {
			/* reached from inside a script callback, e.g. the script
			   drove a PIO and ds2408::pio_set() dispatched
			   ACT_DEV_CHANGE straight back at us */
			if (logger)
				logger->warn(std::format("jsengine: dropping re-entrant action {}", ev.code));
			return 0;
		}
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue fn = JS_GetPropertyStr(ctx, global, "onAction");

		if (JS_IsFunction(ctx, fn)) {
			std::string payload = ev.data ? ev.data->dump() : std::string();
			JSValue argv[3];

			argv[0] = JS_NewInt32(ctx, ev.code);
			argv[1] = JS_NewInt32(ctx, ev.val);
			argv[2] = ev.data
				? JS_ParseJSON(ctx, payload.c_str(), payload.size(), "<event>")
				: JS_NULL;

			JSValue res = call_guarded(fn, global, 3, argv, "onAction");
			if (!JS_IsException(res))
				JS_ToInt32(ctx, &ret, res);
			JS_FreeValue(ctx, res);
			for (int i = 0; i < 3; i++)
				JS_FreeValue(ctx, argv[i]);
		}
		JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, global);

		return ret;
	}
};

// These functions are the "doorway" into the library
extern "C" Plugin* create_plugin() {
	return new JsEngine();
}

extern "C" void destroy_plugin(Plugin* p) {
	delete p;
}
