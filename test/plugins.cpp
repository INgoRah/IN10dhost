#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <csignal>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstdlib> // Required for std::system
#include <thread>

#include "main.h"
#include "ow_devices.h"
#include "plugins.h"

extern OwDevices ow;
extern DS2482 ds;
extern Plugins plugins;

std::filesystem::path exec_path()
{
	char buffer[PATH_MAX];
	ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length > 0) {
		buffer[length] = '\0';
		return std::filesystem::path(buffer).parent_path();
	}
	return std::filesystem::path(".");
}

TEST(plugins, LoadPlugin)
{
	bool ok = false;
	std::string f = std::filesystem::current_path();
	LogLevel lvl = logger.get_level();

	ow.init();
	// Create an empty file to simulate a missing plugin
	string s = std::format("touch {}/libinvalid.so", exec_path().string());
	std::system(s.c_str());
	f = f + "/test/plugin.json";
	try {
		logger.error("loading json with plugin");
		ow.load(f.c_str());
		logger.set_level(lvl);
		logger.error("begin with plugin");
		ow.begin();
		ok = true;
	}
	catch (const std::exception& e) {
		logger.set_level(lvl);
		logger.error(std::format("loading failed {}", e.what()));
		ok = false;
	}
	EXPECT_EQ(ok, true);

}

// Loading the example is a no-op once it is already loaded, so calling
// this keeps each test below independent of suite execution order.
static void ensure_example_loaded()
{
	plugins.load(json{ { "example", json::object() } });
}

TEST(plugins, PluginActions)
{
	ensure_example_loaded();

	// Plugins::action() returns the sum over all loaded plugins, and
	// the example reports 1 for the actions it handles - so a 1 here
	// proves the event actually reached the plugin's action()
	EXPECT_EQ(plugins.action(ACT_READY), 1);

	// ... and 0 for everything it falls through to default on
	EXPECT_EQ(plugins.action(ACT_CFG_LOAD), 0);
	EXPECT_EQ(plugins.action(ACT_PERIODIC_SECOND), 0);
}

TEST(plugins, LoadSkipsAlreadyLoadedPlugin)
{
	ensure_example_loaded();
	int once = plugins.action(ACT_READY);
	EXPECT_EQ(once, 1);

	// a repeated load must not create a second instance - which would
	// show up as a doubled sum from action()
	ensure_example_loaded();
	EXPECT_EQ(plugins.action(ACT_READY), once);
}

TEST(plugins, ActionPayloadIsPassedThrough)
{
	ensure_example_loaded();

	// the example reads rom/pio back out of the payload
	json data = {
		{ "bus", 0 },
		{ "rom", "29.0200FDFF6677F8" },
		{ "pio", 3 }
	};
	EXPECT_EQ(plugins.action(ACT_DEV_CHANGE, 0, &data), 1);

	// data is optional: an action without a payload must be fine too
	EXPECT_EQ(plugins.action(ACT_DEV_CHANGE, 0, nullptr), 1);
	EXPECT_EQ(plugins.action(ACT_DEV_CHANGE, 0), 1);
}

TEST(plugins, PluginExceptionDoesNotEscape)
{
	ensure_example_loaded();

	// "pio" as a string makes the example's value("pio", 0xff) throw a
	// json type_error. Plugins::action() has to contain that: nothing
	// further up (alarm handling, the poll loop, FUSE read/write)
	// catches anything, so an escaping exception would terminate the
	// whole daemon instead of just failing one plugin.
	json bad = {
		{ "rom", "29.0200FDFF6677F8" },
		{ "pio", "not-a-number" }
	};

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE); // the failure is logged, expected
	int ret = -1;
	EXPECT_NO_THROW(ret = plugins.action(ACT_DEV_CHANGE, 0, &bad));
	logger.set_level(lvl);

	// the plugin threw instead of returning, so it contributes nothing
	EXPECT_EQ(ret, 0);

	// and the plugin is still usable for the next event
	EXPECT_EQ(plugins.action(ACT_READY), 1);
}

// The scripting plugin is only built when quickjs was found at configure
// time, so every test below has to cope with it being absent.
static bool jsengine_available()
{
	return std::filesystem::exists(exec_path() / "libjsengine.so");
}

// Loads the jsengine plugin with a script written on the fly.
//
// Plugins::load() skips a plugin that is already loaded and there is no
// API to swap its script afterwards, so every test has to share one
// script that branches on the action code rather than loading its own.
static void load_js_once()
{
	static std::string path;

	if (!path.empty())
		return;
	path = (std::filesystem::current_path() / "test_engine.js").string();
	std::ofstream(path) << R"JS(
		function onAction(code, val, data) {
			switch (code) {
			case 3:                     /* ACT_READY */
				return 7;
			case 8:                     /* ACT_DEV_CHANGE */
				return data ? data.pio : 0;
			case 6:                     /* ACT_ALARM_BEFORE: throws */
				throw new Error("boom");
			case 7:                     /* ACT_ALARM_AFTER: never returns */
				while (true) { }
			}
			return 0;
		}
	)JS";
	plugins.load(json{ { "jsengine", { { "script", path } } } });
}

TEST(plugins, JsEngineRunsScript)
{
	if (!jsengine_available())
		GTEST_SKIP() << "built without quickjs";

	// what the script returns from onAction() is summed into the result
	// along with every other plugin, so compare against a baseline
	// instead of an absolute value
	int base = plugins.action(ACT_READY);
	load_js_once();
	EXPECT_EQ(plugins.action(ACT_READY), base + 7);

	// the payload really arrives as a JS object: the script reads
	// data.pio back out of it and returns that
	int dev_base = plugins.action(ACT_DEV_CHANGE, 0, nullptr);
	json data = { { "rom", "29.0200FDFF6677F8" }, { "pio", 5 } };
	EXPECT_EQ(plugins.action(ACT_DEV_CHANGE, 0, &data), dev_base + 5);
}

TEST(plugins, JsEngineFromOtherThread)
{
	if (!jsengine_available())
		GTEST_SKIP() << "built without quickjs";

	load_js_once();

	// The engine is created on the thread that loads the config, but
	// the daemon dispatches from the background poll worker and from
	// the FUSE threads. QuickJS derives its stack limit from the stack
	// pointer it saw at JS_NewRuntime(), so unless that top is re-based
	// per thread every call from another thread fails with "Maximum
	// call stack size exceeded".
	int base = plugins.action(ACT_READY);
	int other = 0;

	std::thread t([&] { other = plugins.action(ACT_READY); });
	t.join();

	EXPECT_EQ(other, base);
}

TEST(plugins, JsEngineSurvivesBadScript)
{
	if (!jsengine_available())
		GTEST_SKIP() << "built without quickjs";

	load_js_once();
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE); // the script errors are expected

	// a script that throws must not escape into the daemon ...
	EXPECT_NO_THROW(plugins.action(ACT_ALARM_BEFORE));
	// ... and the engine stays usable for the next event
	EXPECT_NO_THROW(plugins.action(ACT_READY));

	// an endless loop is cut off by the engine's interrupt handler
	// instead of hanging the daemon; if that regresses this hangs
	auto start = std::chrono::steady_clock::now();
	EXPECT_NO_THROW(plugins.action(ACT_ALARM_AFTER));
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
	// it really ran and really got interrupted, rather than the script
	// never having been reached at all
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);

	logger.set_level(lvl);
}

TEST(plugins, ConfigRoundTrip)
{
	ensure_example_loaded();

	json saved = plugins.save();
	ASSERT_TRUE(saved.contains("example"));
	// what Example::config_get() reports
	EXPECT_EQ(saved["example"].value("enabled", false), true);
}

TEST(plugins, PluginSave)
{
	std::string f = std::filesystem::current_path();

	// set log level back if changed by test
	// check plugin actually saved config
	ow.save(f);
	ow.begin();
}

// Reloads the fault injection plugin with a given mode. load() skips a
// plugin that is already loaded, so it has to be removed first.
static void load_faulty(const string& mode)
{
	plugins.remove("faulty");
	plugins.load(json{ { "faulty", { { "mode", mode } } } });
}

TEST(plugins, FaultyPluginActionThrows)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE); // the failures are the point

	// a std::exception out of action() is caught and the other plugins
	// still run, so the result is whatever they returned
	ensure_example_loaded();
	load_faulty("action");
	int expect = plugins.action(ACT_READY);   // faulty contributes 0
	EXPECT_NO_THROW(plugins.action(ACT_READY));

	// something that is not a std::exception at all has to be stopped by
	// the catch(...) rather than reaching the daemon
	load_faulty("action_odd");
	int ret = -1;
	EXPECT_NO_THROW(ret = plugins.action(ACT_READY));
	EXPECT_EQ(ret, expect);

	logger.set_level(lvl);
	plugins.remove("faulty");
}

TEST(plugins, FaultyPluginExitThrows)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);

	// exit() runs while unloading; throwing there must not stop the
	// unload from completing
	load_faulty("exit");
	size_t before = plugins.count();
	EXPECT_NO_THROW(EXPECT_EQ(plugins.remove("faulty"), 0));
	EXPECT_EQ(plugins.count(), before - 1);

	logger.set_level(lvl);
}

TEST(plugins, FaultyPluginConfigGetThrows)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);

	// config_of() is what serves a read of settings/plugins/<name>, so a
	// throwing config_get() must degrade to an empty object, not escape
	load_faulty("config_get");
	json cfg;
	EXPECT_NO_THROW(cfg = plugins.config_of("faulty"));
	EXPECT_TRUE(cfg.is_object());
	EXPECT_TRUE(cfg.empty());

	logger.set_level(lvl);
	plugins.remove("faulty");
}

TEST(plugins, FaultyPluginConfigSetThrows)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);

	// reload() hands an unchanged plugin its config back; throwing from
	// config_set() there must not abort the whole reload
	load_faulty("config_set");
	ensure_example_loaded();
	EXPECT_NO_THROW(plugins.reload());
	// the rest of the plugins are still loaded and working
	EXPECT_EQ(plugins.action(ACT_READY), plugins.action(ACT_READY));

	logger.set_level(lvl);
	plugins.remove("faulty");
}

TEST(plugins, FaultyPluginThrowsNonException)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);

	// Everything that crosses into a plugin has a catch(...) behind the
	// catch(std::exception); these drive that second arm by throwing
	// something that is not derived from std::exception at all.
	load_faulty("config_get_odd");
	json cfg;
	EXPECT_NO_THROW(cfg = plugins.config_of("faulty"));
	EXPECT_TRUE(cfg.is_object());
	EXPECT_TRUE(cfg.empty());

	// config_set: reload() re-applies the config of an unchanged plugin
	load_faulty("config_set_odd");
	EXPECT_NO_THROW(plugins.reload("faulty"));

	// exit: thrown while unloading, the unload still has to finish
	load_faulty("exit_odd");
	size_t before = plugins.count();
	EXPECT_NO_THROW(EXPECT_EQ(plugins.remove("faulty"), 0));
	EXPECT_EQ(plugins.count(), before - 1);

	logger.set_level(lvl);
}

TEST(plugins, ReloadOnlyTouchesTheNamedPlugin)
{
	ensure_example_loaded();
	load_faulty("");

	// with a name, every other loaded plugin is skipped outright
	EXPECT_EQ(plugins.reload("example"), 0);
	EXPECT_EQ(plugins.count(), plugins.names().size());
	// a name that is not loaded reloads nothing at all
	EXPECT_EQ(plugins.reload("no_such_plugin"), 0);

	plugins.remove("faulty");
}

TEST(plugins, LoadPluginMissingSymbols)
{
	// A valid shared object with no create_plugin/destroy_plugin in it:
	// dlopen() succeeds and dlsym() is what fails, which is a different
	// path from libinvalid.so above (that one fails in dlopen).
	std::filesystem::path so = exec_path() / "libnosym.so";
	string cmd = std::format(
		"echo 'int dummy;' | g++ -shared -fPIC -x c++ - -o {} 2>/dev/null",
		so.string());
	if (std::system(cmd.c_str()) != 0 || !std::filesystem::exists(so))
		GTEST_SKIP() << "could not build the stub library";

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	EXPECT_EQ(plugins.add("nosym"), -1);
	logger.set_level(lvl);

	// nothing was registered
	EXPECT_FALSE(plugins.save().contains("nosym"));
	std::filesystem::remove(so);
}

TEST(plugins, ReloadFailureDropsPlugin)
{
	ensure_example_loaded();
	std::filesystem::path lib = exec_path() / "libexample.so";
	std::filesystem::path backup = exec_path() / "libexample.so.orig";
	std::filesystem::copy_file(lib, backup,
		std::filesystem::copy_options::overwrite_existing);

	// replace it with something that is not loadable at all: the hash
	// differs so reload() tries, the unload succeeds, and loading the
	// replacement fails
	std::ofstream(lib, std::ios::binary | std::ios::trunc) << "not an ELF";

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	EXPECT_EQ(plugins.reload("example"), 0);   // nothing came back
	logger.set_level(lvl);
	EXPECT_FALSE(plugins.save().contains("example"));

	std::filesystem::copy_file(backup, lib,
		std::filesystem::copy_options::overwrite_existing);
	std::filesystem::remove(backup);
	ensure_example_loaded();                   // restore for other tests
	EXPECT_TRUE(plugins.save().contains("example"));
}

TEST(plugins, LoadPluginCopyFailure)
{
	// plugin_init() locates the plugin via fs::exists(), which is also
	// true for a directory; fs::copy_file() then throws because its
	// source has to be a regular file, driving plugin_init()'s catch
	// block instead of its usual dlopen()/dlsym() failure paths
	std::filesystem::path broken = exec_path() / "libbroken_dir.so";
	std::error_code ec;
	std::filesystem::create_directory(broken, ec);
	ASSERT_FALSE(ec);

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	json j = { { "broken_dir", json::object() } };
	// must not throw out of load(): plugin_init() catches internally
	EXPECT_NO_THROW(plugins.load(j));
	logger.set_level(lvl);

	// the failed plugin was never registered
	json saved = plugins.save();
	EXPECT_FALSE(saved.contains("broken_dir"));

	std::filesystem::remove(broken, ec);
}

TEST(plugins, AddAndRemove)
{
	ensure_example_loaded();
	size_t before = plugins.count();

	// already loaded, so nothing to do
	EXPECT_EQ(plugins.add("example"), 1);
	EXPECT_EQ(plugins.count(), before);

	// unloading really detaches it: the action no longer reaches it
	int with = plugins.action(ACT_READY);
	EXPECT_EQ(plugins.remove("example"), 0);
	EXPECT_EQ(plugins.count(), before - 1);
	EXPECT_EQ(plugins.action(ACT_READY), with - 1);

	// removing something that is not loaded is an error, not a crash
	EXPECT_EQ(plugins.remove("example"), -1);
	EXPECT_EQ(plugins.remove("never_existed"), -1);

	// and it can be brought back
	EXPECT_EQ(plugins.add("example"), 0);
	EXPECT_EQ(plugins.count(), before);
	EXPECT_EQ(plugins.action(ACT_READY), with);
}

TEST(plugins, AddMissingPluginFails)
{
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	EXPECT_EQ(plugins.add("no_such_plugin"), -1);
	logger.set_level(lvl);
}

TEST(plugins, ReloadIgnoresUnchangedLibrary)
{
	ensure_example_loaded();
	int before = plugins.action(ACT_READY);

	// touching the library changes its mtime but not its contents, and
	// the hash is what decides - so nothing must be reloaded
	std::filesystem::path lib = exec_path() / "libexample.so";
	std::filesystem::last_write_time(lib, std::filesystem::file_time_type::clock::now());

	EXPECT_EQ(plugins.reload(), 0);
	// still loaded and still working
	EXPECT_EQ(plugins.action(ACT_READY), before);
}

TEST(plugins, ReloadReplacesChangedLibrary)
{
	ensure_example_loaded();
	std::filesystem::path lib = exec_path() / "libexample.so";
	std::filesystem::path backup = exec_path() / "libexample.so.orig";
	int before = plugins.action(ACT_READY);
	size_t count = plugins.count();

	// stand in for a rebuilt plugin: a trailing byte changes the
	// content hash without stopping the ELF from loading
	std::filesystem::copy_file(lib, backup,
		std::filesystem::copy_options::overwrite_existing);
	{
		std::ofstream out(lib, std::ios::binary | std::ios::app);
		out.put('\0');
	}

	EXPECT_EQ(plugins.reload(), 1);                 // exactly one reloaded
	EXPECT_EQ(plugins.count(), count);              // still registered
	EXPECT_EQ(plugins.action(ACT_READY), before);   // and still working

	std::filesystem::copy_file(backup, lib,
		std::filesystem::copy_options::overwrite_existing);
	std::filesystem::remove(backup);
	plugins.reload();  // pick the original back up
}

TEST(plugins, ReloadPicksUpEditedScript)
{
	if (!jsengine_available())
		GTEST_SKIP() << "built without quickjs";

	load_js_once();
	std::string path = (std::filesystem::current_path() / "test_engine.js").string();
	int before = plugins.action(ACT_READY);

	// an untouched script must not be re-evaluated, so the result stays
	EXPECT_EQ(plugins.reload(), 0);
	EXPECT_EQ(plugins.action(ACT_READY), before);

	// now edit it: the library is unchanged, so reload() reports no
	// library reloads, but handing the config back makes the engine
	// notice the new contents and re-evaluate them
	std::ofstream(path) << R"JS(
		function onAction(code, val, data) {
			return code == 3 ? 9 : 0;
		}
	)JS";
	EXPECT_EQ(plugins.reload(), 0);
	EXPECT_EQ(plugins.action(ACT_READY), before - 7 + 9);
}
