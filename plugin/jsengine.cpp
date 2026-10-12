/*
 * QuickJS scripting plugin.
 *
 * Loads the .js files listed under the "scripts" key of its plugin
 * config (or the one under "script") and forwards every ActionEvent to
 * a global onAction(code, val, data) function in each, in the order of
 * the list, adding up what they return. data is the event payload as a
 * plain JS object, or null when the action carries none.
 *
 *   "jsengine": { "scripts": [ "/opt/lib/in10dfs/a.js", "/opt/lib/in10dfs/b.js" ] }
 *
 * Every script runs in a context of its own: they do not see each
 * other's globals, so each can define its own onAction, onRead and
 * onWrite, and a script that fails to load or throws leaves the others
 * running.
 *
 * Scripts reach the 1-Wire devices through the global "ow" object:
 *   ow.log(msg)              write to the daemon log
 *   ow.pio_set(rom, pio)     drive all ds2408 PIOs at once (raw output
 *                            latch byte), returns true on success
 *   ow.pin_switch(rom, pin, state[, level])
 *                            switch one ds2408 pin, state is ow.ON,
 *                            ow.OFF or ow.TOGGLE; a PWM pin takes level
 *   ow.level_set(rom, pin, level)
 *                            set a ds2408 PWM pin to level (0 = off)
 *   ow.brightness_set(rom, value)
 *                            hand a ds2408 the current brightness, for
 *                            devices without a light sensor of their own
 *   ow.threshold_set(rom, value)
 *                            brightness above which a ds2408 does not
 *                            switch its timed lights
 *                            all four return true on success
 *   ow.temp(rom)             read a ds1820 temperature, null if not one
 *   ow.volt(rom, channel)    read a cached ds2450 voltage
 *   ow.pins([role])          the pins with that role ("light", ...), or
 *                            with any role: [{rom, pin, name, role,
 *                            room, output, on}, ...]
 *   ow.role_switch(role, on) switch every output with that role on or
 *                            off, true when all of them switched
 *   ow.file_add(path, size)  add a file to the file system, e.g.
 *                            "status/my_stuff" (see IPluginFiles),
 *                            true when it could be added
 *   ow.file_remove(path)     and remove it again
 *
 * Reading one of these files calls onRead(path) in the script, what it
 * returns is the file's content (null or undefined: not readable).
 * Writing calls onWrite(path, data) with the written text, returning
 * false refuses it; without onWrite the files are read only. A reload
 * of the script drops its files, the new version adds its own.
 *
 * A script is reloaded on its own when its file changes on disk: every
 * event first compares the file's modification time, and on a change
 * its contents, with what was last loaded; the other scripts keep
 * running untouched. ACT_PERIODIC_SECOND arrives
 * every second, so an edit is live within about a second. A reload
 * starts the script from a clean global object, like loading it anew.
 *
 * rom is the dotted string form ("29.0200FDFF6677F8") that the event
 * payloads also carry - JS numbers cannot hold a 64 bit rom code
 * exactly, so the string is the only safe round trip.
 */
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
	/* One script file, run in a context of its own: scripts do not see
	   each other's globals, and each is loaded and reloaded on its own */
	struct Script {
		JsEngine* engine;
		std::string path;
		JSContext* ctx = nullptr;
		/* contents as last loaded, so a reload with an unedited file
		   is a no-op instead of wiping the script's state */
		uint64_t hash = 0;
		/* modification time as last loaded, so most events only cost a
		   stat() rather than hashing the file */
		std::filesystem::file_time_type mtime{};
		/* the files it added, dropped when it is reloaded */
		std::set<std::string> files;
	};
	/* in the order of the config, which is the order events reach them;
	   by pointer, each context's opaque points at its Script */
	std::vector<std::unique_ptr<Script>> scripts;
	/* ACT_INITIALIZED and ACT_READY as far as they came, in order: a
	   script reloaded later gets them again, see replay() */
	std::vector<int> seen;
	/* QuickJS is single threaded, but action() is reached both from the
	   FUSE threads and from the background poll worker */
	std::recursive_mutex mtx;
	/* a script that drives a PIO from its ACT_DEV_CHANGE handler would
	   otherwise re-enter through ds2408::pio_set() without end */
	int depth;
	std::chrono::steady_clock::time_point deadline;

	static Script* script_of(JSContext* ctx)
	{
		return (Script*)JS_GetContextOpaque(ctx);
	}

	static JsEngine* engine_of(JSContext* ctx)
	{
		Script* sc = script_of(ctx);

		return sc ? sc->engine : nullptr;
	}

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
		JsEngine* self = engine_of(ctx);

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
		JsEngine* self = engine_of(ctx);

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

	/* Reads argv[i] as a byte. Throws a JS RangeError, and returns
	   false, for anything outside 0..255. */
	static bool arg_u8(JSContext* ctx, JSValueConst v, const char* what, uint8_t& out)
	{
		int32_t n;

		if (JS_ToInt32(ctx, &n, v) < 0)
			return false;
		if (n < 0 || n > 255) {
			JS_ThrowRangeError(ctx, "%s must be 0..255, got %d", what, n);
			return false;
		}
		out = (uint8_t)n;
		return true;
	}

	/* pin numbers are 0..7, anything else would index past the
	   device's per-pin config */
	static bool arg_pin(JSContext* ctx, JSValueConst v, uint8_t& out)
	{
		int32_t n;

		if (JS_ToInt32(ctx, &n, v) < 0)
			return false;
		if (n < 0 || n > 7) {
			JS_ThrowRangeError(ctx, "pin must be 0..7, got %d", n);
			return false;
		}
		out = (uint8_t)n;
		return true;
	}

	static JSValue js_pin_switch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		uint8_t pin, lvl = 0;
		int32_t state;

		if (argc < 3)
			return JS_FALSE;
		IDS2408* d = dynamic_cast<IDS2408*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_FALSE;
		if (!arg_pin(ctx, argv[1], pin))
			return JS_EXCEPTION;
		if (JS_ToInt32(ctx, &state, argv[2]) < 0)
			return JS_EXCEPTION;
		if (state != OFF && state != ON && state != TOGGLE)
			return JS_ThrowRangeError(ctx, "state must be ow.ON, ow.OFF or ow.TOGGLE");
		if (argc > 3 && !arg_u8(ctx, argv[3], "level", lvl))
			return JS_EXCEPTION;

		return JS_NewBool(ctx, d->pin_switch(pin, (enum _pio_mode)state, lvl) == 0);
	}

	static JSValue js_level_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		uint8_t pin, lvl;

		if (argc < 3)
			return JS_FALSE;
		IDS2408* d = dynamic_cast<IDS2408*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_FALSE;
		if (!arg_pin(ctx, argv[1], pin) || !arg_u8(ctx, argv[2], "level", lvl))
			return JS_EXCEPTION;

		return JS_NewBool(ctx, d->level_set(pin, lvl) == 0xAA);
	}

	static JSValue js_brightness_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		uint8_t val;

		if (argc < 2)
			return JS_FALSE;
		IDS2408* d = dynamic_cast<IDS2408*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_FALSE;
		if (!arg_u8(ctx, argv[1], "brightness", val))
			return JS_EXCEPTION;

		return JS_NewBool(ctx, d->brightness_set(val) == 0);
	}

	static JSValue js_threshold_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		uint8_t val;

		if (argc < 2)
			return JS_FALSE;
		IDS2408* d = dynamic_cast<IDS2408*>(arg_dev(ctx, argc, argv));
		if (d == nullptr)
			return JS_FALSE;
		if (!arg_u8(ctx, argv[1], "threshold", val))
			return JS_EXCEPTION;

		return JS_NewBool(ctx, d->threshold_set(val) == 0);
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

	/* Reads v as a role name, any case. Throws a JS RangeError, and
	   returns false, for an unknown one. */
	static bool arg_role(JSContext* ctx, JSValueConst v, PinRole& out)
	{
		const char* s = JS_ToCString(ctx, v);

		if (s == nullptr)
			return false;
		auto r = pin_role_parse(s);
		if (!r)
			JS_ThrowRangeError(ctx, "unknown role '%s'", s);
		JS_FreeCString(ctx, s);
		if (!r)
			return false;
		out = *r;
		return true;
	}

	static JSValue js_pins(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		JsEngine* self = engine_of(ctx);
		PinRole role = PinRole::none;

		if (argc > 0 && !JS_IsUndefined(argv[0]) && !arg_role(ctx, argv[0], role))
			return JS_EXCEPTION;
		JSValue arr = JS_NewArray(ctx);
		if (self == nullptr || self->devices == nullptr)
			return arr;
		uint32_t i = 0;
		for (const auto& p : self->devices->pins(role)) {
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "rom", JS_NewString(ctx, p.rom.c_str()));
			JS_SetPropertyStr(ctx, o, "pin", JS_NewInt32(ctx, p.pin));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, p.name.c_str()));
			JS_SetPropertyStr(ctx, o, "role", JS_NewString(ctx, pin_role_str(p.role)));
			JS_SetPropertyStr(ctx, o, "room", JS_NewString(ctx, p.room.c_str()));
			JS_SetPropertyStr(ctx, o, "output", JS_NewBool(ctx, p.output));
			JS_SetPropertyStr(ctx, o, "on", JS_NewBool(ctx, p.on));
			JS_SetPropertyUint32(ctx, arr, i++, o);
		}

		return arr;
	}

	static JSValue js_role_switch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		JsEngine* self = engine_of(ctx);
		PinRole role;

		if (argc < 2 || self == nullptr || self->devices == nullptr)
			return JS_FALSE;
		if (!arg_role(ctx, argv[0], role))
			return JS_EXCEPTION;
		int on = JS_ToBool(ctx, argv[1]);
		if (on < 0)
			return JS_EXCEPTION;

		return JS_NewBool(ctx, self->devices->role_switch(role, on != 0) == 0);
	}

	static JSValue js_file_add(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		JsEngine* self = engine_of(ctx);
		int32_t size;

		if (argc < 2 || self == nullptr || self->files == nullptr)
			return JS_FALSE;
		if (JS_ToInt32(ctx, &size, argv[1]) < 0)
			return JS_EXCEPTION;
		const char* path = JS_ToCString(ctx, argv[0]);
		if (path == nullptr)
			return JS_EXCEPTION;
		/* the host knows the files by plugin, the scripts of this one
		   must not take each other's either */
		Script* sc = script_of(ctx);
		bool ok = self->script_owning(path) == nullptr || self->script_owning(path) == sc;
		ok = ok && self->files->file_add(self, path, size);
		if (ok)
			sc->files.insert(path);
		JS_FreeCString(ctx, path);

		return JS_NewBool(ctx, ok);
	}

	static JSValue js_file_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		JsEngine* self = engine_of(ctx);

		if (argc < 1 || self == nullptr || self->files == nullptr)
			return JS_FALSE;
		const char* path = JS_ToCString(ctx, argv[0]);
		if (path == nullptr)
			return JS_EXCEPTION;
		Script* sc = script_of(ctx);
		bool ok = sc->files.erase(path) > 0 && self->files->file_remove(self, path);
		JS_FreeCString(ctx, path);

		return JS_NewBool(ctx, ok);
	}

	/* Runs periodically while script code executes; a non zero return
	   aborts it, which is what stops a runaway loop from hanging the
	   whole daemon. */
	static int js_interrupt(JSRuntime*, void* opaque)
	{
		JsEngine* self = (JsEngine*)opaque;

		return std::chrono::steady_clock::now() > self->deadline ? 1 : 0;
	}

	void log_exception(JSContext* ctx, const char* what)
	{
		JSValue e = JS_GetException(ctx);
		const char* msg = JS_ToCString(ctx, e);

		if (logger)
			logger->error(std::format("jsengine: {} failed: {}", what, msg ? msg : "?"));
		if (msg)
			JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, e);
	}

	/* The runtime measures stack depth against the stack top of the
	   thread that last set it, but calls arrive from the background
	   poll worker and the FUSE threads alike. Every entry point calls
	   this first, before *any* QuickJS call - JS_ParseJSON() and
	   JS_Eval() check the stack too, not only running the script - or
	   a call from another thread fails with "Maximum call stack size
	   exceeded". Safe because the mutex means only one thread is ever
	   inside the engine. */
	void enter()
	{
		if (rt)
			JS_UpdateStackTop(rt);
	}

	/* Arms the watchdog, runs fn, and reports anything it threw. */
	JSValue call_guarded(JSContext* ctx, JSValue fn, JSValue self_obj, int argc, JSValue* argv,
		const char* what)
	{
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(SCRIPT_TIMEOUT_MS);
		depth++;
		JSValue res = JS_Call(ctx, fn, self_obj, argc, argv);
		depth--;
		if (JS_IsException(res))
			log_exception(ctx, what);

		return res;
	}

	void register_globals(JSContext* ctx)
	{
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue owobj = JS_NewObject(ctx);

		JS_SetPropertyStr(ctx, owobj, "log",
			JS_NewCFunction(ctx, js_log, "log", 1));
		JS_SetPropertyStr(ctx, owobj, "pio_set",
			JS_NewCFunction(ctx, js_pio_set, "pio_set", 2));
		JS_SetPropertyStr(ctx, owobj, "pin_switch",
			JS_NewCFunction(ctx, js_pin_switch, "pin_switch", 4));
		JS_SetPropertyStr(ctx, owobj, "level_set",
			JS_NewCFunction(ctx, js_level_set, "level_set", 3));
		JS_SetPropertyStr(ctx, owobj, "brightness_set",
			JS_NewCFunction(ctx, js_brightness_set, "brightness_set", 2));
		JS_SetPropertyStr(ctx, owobj, "threshold_set",
			JS_NewCFunction(ctx, js_threshold_set, "threshold_set", 2));
		/* states for pin_switch(), the values of enum _pio_mode */
		JS_SetPropertyStr(ctx, owobj, "OFF", JS_NewInt32(ctx, OFF));
		JS_SetPropertyStr(ctx, owobj, "ON", JS_NewInt32(ctx, ON));
		JS_SetPropertyStr(ctx, owobj, "TOGGLE", JS_NewInt32(ctx, TOGGLE));
		JS_SetPropertyStr(ctx, owobj, "temp",
			JS_NewCFunction(ctx, js_temp, "temp", 1));
		JS_SetPropertyStr(ctx, owobj, "volt",
			JS_NewCFunction(ctx, js_volt, "volt", 2));
		JS_SetPropertyStr(ctx, owobj, "pins",
			JS_NewCFunction(ctx, js_pins, "pins", 1));
		JS_SetPropertyStr(ctx, owobj, "role_switch",
			JS_NewCFunction(ctx, js_role_switch, "role_switch", 2));
		JS_SetPropertyStr(ctx, owobj, "file_add",
			JS_NewCFunction(ctx, js_file_add, "file_add", 2));
		JS_SetPropertyStr(ctx, owobj, "file_remove",
			JS_NewCFunction(ctx, js_file_remove, "file_remove", 1));
		JS_SetPropertyStr(ctx, global, "ow", owobj);
		JS_FreeValue(ctx, global);
	}

	/* the script that added path, nullptr if none did */
	Script* script_owning(const std::string& path)
	{
		for (auto& sc : scripts)
			if (sc->files.count(path))
				return sc.get();
		return nullptr;
	}

	/* the files of a script that goes away with its context */
	void drop_files(Script& sc)
	{
		if (files)
			for (const auto& f : sc.files)
				files->file_remove(this, f);
		sc.files.clear();
	}

	void free_context(Script& sc)
	{
		drop_files(sc);
		if (sc.ctx) {
			JS_FreeContext(sc.ctx);
			sc.ctx = nullptr;
		}
	}

	/* Throws away the script's context and builds a fresh one, so a
	   reloaded script starts from a clean global object instead of
	   inheriting whatever the previous version defined. The runtime is
	   shared and kept. */
	bool reset_context(Script& sc)
	{
		free_context(sc);
		sc.ctx = JS_NewContext(rt);
		if (sc.ctx == nullptr) {
			if (logger)
				logger->error(std::format("jsengine: cannot create context for {}", sc.path));
			return false;
		}
		JS_SetContextOpaque(sc.ctx, &sc);
		register_globals(sc.ctx);

		return true;
	}

	void load_script(Script& sc)
	{
		std::ifstream f(sc.path);

		if (!f) {
			if (logger)
				logger->error(std::format("jsengine: cannot open script {}", sc.path));
			return;
		}
		std::stringstream buf;
		buf << f.rdbuf();
		std::string src = buf.str();

		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(SCRIPT_TIMEOUT_MS);
		JSValue res = JS_Eval(sc.ctx, src.c_str(), src.size(), sc.path.c_str(),
			JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(res))
			log_exception(sc.ctx, sc.path.c_str());
		else if (logger)
			logger->info(std::format("jsengine: loaded {}", sc.path));
		JS_FreeValue(sc.ctx, res);
	}

	/* A script reloaded after the start missed ACT_INITIALIZED and
	   ACT_READY, which is where a script typically sets itself up (adds
	   its files, ...). They are handed to it again, to it alone. */
	void replay(Script& sc)
	{
		for (int code : seen)
			script_action(sc, ActionEvent{ code, 0, nullptr });
	}

	/* (Re)loads a script from its file as it is now */
	void start_script(Script& sc)
	{
		std::error_code ec;

		sc.hash = file_hash(sc.path);
		sc.mtime = std::filesystem::last_write_time(sc.path, ec);
		if (reset_context(sc))
			load_script(sc);
	}

	/* Reloads the script if the file changed on disk since it was last
	   loaded. A missing file (an editor in the middle of replacing it)
	   is left for the next event to look at. */
	void check_script(Script& sc)
	{
		std::error_code ec;

		auto mtime = std::filesystem::last_write_time(sc.path, ec);
		if (ec || mtime == sc.mtime)
			return;
		sc.mtime = mtime;
		/* touched or saved unchanged: keep the running script's state */
		uint64_t hash = file_hash(sc.path);
		if (hash == 0 || hash == sc.hash)
			return;
		sc.hash = hash;
		if (logger)
			logger->info(std::format("jsengine: {} changed, reloading", sc.path));
		if (reset_context(sc)) {
			load_script(sc);
			replay(sc);
		}
	}

	/* Calls the script function name(path[, data]) for a file of a
	   script. Sets res, the caller frees it in ctx; an errno when there
	   is nothing to call or it threw. */
	int call_file(const char* name, const string& path, const char* data, size_t size,
		JSValue& res, JSContext*& ctx)
	{
		if (depth > 0)
			return -EBUSY;
		enter();
		Script* sc = script_owning(path);
		if (sc == nullptr)
			return -ENOENT;
		/* an edited script is loaded before, like for an action; that
		   may well have dropped this file */
		check_script(*sc);
		if (sc->ctx == nullptr || sc->files.count(path) == 0)
			return -ENOENT;
		ctx = sc->ctx;
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue fn = JS_GetPropertyStr(ctx, global, name);
		int err = 0;

		if (JS_IsFunction(ctx, fn)) {
			JSValue argv[2];
			argv[0] = JS_NewString(ctx, path.c_str());
			argv[1] = data ? JS_NewStringLen(ctx, data, size) : JS_UNDEFINED;
			res = call_guarded(ctx, fn, global, data ? 2 : 1, argv, name);
			if (JS_IsException(res)) {
				JS_FreeValue(ctx, res);
				err = -EIO;
			}
			JS_FreeValue(ctx, argv[0]);
			JS_FreeValue(ctx, argv[1]);
		} else {
			err = -EACCES;
		}
		JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, global);

		return err;
	}

	/* onAction() of one script, 0 when it has none or it threw */
	int32_t script_action(Script& sc, const ActionEvent& ev)
	{
		int32_t ret = 0;

		check_script(sc);
		if (sc.ctx == nullptr)
			return 0;
		JSContext* ctx = sc.ctx;
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
			if (JS_IsException(argv[2])) {
				/* report it here rather than hand the script an
				   exception object as its payload */
				log_exception(ctx, "event payload");
				argv[2] = JS_NULL;
			}

			JSValue res = call_guarded(ctx, fn, global, 3, argv, "onAction");
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

public:
	JsEngine() : logger(nullptr), devices(nullptr), rt(nullptr), depth(0) {}

	void info() override {
		std::cout << "QuickJS engine " << JS_GetVersion() << std::endl;
	}

	/* only the runtime, the scripts come with the config */
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
		logger->debug("jsengine: initialized");
	}

	void exit() override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);

		/* freeing runs the garbage collector, also stack checked */
		enter();
		for (auto& sc : scripts)
			free_context(*sc);
		scripts.clear();
		if (rt) {
			JS_FreeRuntime(rt);
			rt = nullptr;
		}
		logger = nullptr;
		devices = nullptr;
	}

	/* "script" for a single one, as configs before there were several */
	json config_get() const override
	{
		json j;

		if (scripts.size() == 1) {
			j["script"] = scripts[0]->path;
		} else {
			j["scripts"] = json::array();
			for (const auto& sc : scripts)
				j["scripts"].push_back(sc->path);
		}
		return j;
	}

	/* "scripts": a list of files, or "script": one. Also the reload
	   path: Plugins::reload() hands an unchanged plugin its own config
	   back so it can re-read external files. A script that stays in the
	   list keeps running, with its state, unless its file changed; one
	   that is no longer in it is stopped. */
	void config_set(const json& j) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		std::vector<std::string> want;

		if (rt == nullptr)
			return;
		if (j.contains("scripts")) {
			for (const auto& p : j.at("scripts"))
				if (p.is_string() && !p.get<std::string>().empty())
					want.push_back(p.get<std::string>());
		} else if (!j.value("script", "").empty()) {
			want.push_back(j.value("script", ""));
		} else {
			return;
		}
		enter();
		/* the new list in place before any script starts, so a script
		   adding a file sees those of every other one */
		std::vector<std::unique_ptr<Script>> next;
		std::vector<Script*> start;
		for (const auto& path : want) {
			if (std::any_of(next.begin(), next.end(),
					[&](const auto& sc) { return sc->path == path; }))
				continue;
			auto it = std::find_if(scripts.begin(), scripts.end(),
				[&](const auto& sc) { return sc && sc->path == path; });
			if (it != scripts.end()) {
				next.push_back(std::move(*it));
				/* same contents: leave it and its state alone rather
				   than re-evaluating it for nothing */
				if (next.back()->ctx == nullptr || file_hash(path) != next.back()->hash)
					start.push_back(next.back().get());
				continue;
			}
			next.push_back(std::make_unique<Script>());
			next.back()->engine = this;
			next.back()->path = path;
			start.push_back(next.back().get());
		}
		/* what is left was taken out of the list */
		for (auto& sc : scripts)
			if (sc) {
				if (logger)
					logger->info(std::format("jsengine: stopping {}", sc->path));
				free_context(*sc);
			}
		scripts = std::move(next);
		for (Script* sc : start) {
			start_script(*sc);
			replay(*sc);
		}
	}

	/* every script in turn, the results added up like the host does
	   for the plugins */
	int action(const ActionEvent& ev) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		int ret = 0;

		if (depth > 0) {
			/* reached from inside a script callback, e.g. the script
			   drove a PIO and ds2408::pio_set() dispatched
			   ACT_DEV_CHANGE straight back at us */
			if (logger)
				logger->warn(std::format("jsengine: dropping re-entrant action {}", ev.code));
			return 0;
		}
		enter();
		for (auto& sc : scripts)
			ret += script_action(*sc, ev);
		// after sending it, or a script reloaded during it got it twice
		if ((ev.code == ACT_INITIALIZED || ev.code == ACT_READY) &&
				std::find(seen.begin(), seen.end(), ev.code) == seen.end())
			seen.push_back(ev.code);

		return ret;
	}

	int fs_read(const string& path, char* buf, size_t size) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		JSValue res;
		JSContext* ctx = nullptr;
		int err = call_file("onRead", path, nullptr, 0, res, ctx);

		if (err)
			return err;
		if (JS_IsNull(res) || JS_IsUndefined(res)) {
			JS_FreeValue(ctx, res);
			return -EACCES;
		}
		/* anything else as text, a number reads as "42" */
		size_t len;
		const char* str = JS_ToCStringLen(ctx, &len, res);
		JS_FreeValue(ctx, res);
		if (str == nullptr) {
			log_exception(ctx, "onRead result");
			return -EIO;
		}
		len = std::min(len, size);
		std::memcpy(buf, str, len);
		JS_FreeCString(ctx, str);

		return (int)len;
	}

	int fs_write(const string& path, const char* buf, size_t size) override
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		JSValue res;
		JSContext* ctx = nullptr;
		int err = call_file("onWrite", path, buf, size, res, ctx);

		if (err)
			return err;
		bool refused = JS_IsBool(res) && !JS_ToBool(ctx, res);
		JS_FreeValue(ctx, res);

		return refused ? -EINVAL : (int)size;
	}
};

// These functions are the "doorway" into the library
extern "C" Plugin* create_plugin() {
	return new JsEngine();
}

extern "C" void destroy_plugin(Plugin* p) {
	delete p;
}
