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
	logger.set_level(LogLevel::NONE);
	try {
		ow.load(f.c_str());
		logger.set_level(lvl);
		ok = true;
	}
	catch (const std::exception& e) {
		logger.set_level(lvl);
		logger.error(std::format("loading failed {}", e.what()));
		ok = false;
	}
	EXPECT_EQ(ok, true);
}

TEST(plugins, PluginSave)
{
	std::string f = std::filesystem::current_path();

	// set log level back if changed by test
	// check plugin actually saved config
	ow.save(f);
	ow.begin(&ds);
	plugins.action(2, 0); // initialized
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
