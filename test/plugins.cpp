#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <csignal>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstdlib> // Required for std::system

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
		ow.begin(&ds);
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
	ow.begin(&ds);
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

TEST(plugins, UnusedManagementStubs)
{
	// reload/add/remove are currently unimplemented no-ops; exercised
	// here so a real implementation later has a place to add behavior
	EXPECT_EQ(plugins.reload(), 0);
	EXPECT_EQ(plugins.add("example"), 0);
	EXPECT_EQ(plugins.remove("example"), 0);
}
