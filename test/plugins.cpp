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
	buffer[length] = '\0';
	return std::filesystem::path(buffer).parent_path();
}

TEST(plugins, LoadPlugin)
{
	bool ok = false;
	std::string f = std::filesystem::current_path();

	ow.init();
	// Create an empty file to simulate a missing plugin
	string s = std::format("touch {}/libinvalid.so", exec_path().string());
	std::system(s.c_str());
	f = f + "/test/plugin.json";
	try {
		ow.load(f.c_str());
		ok = true;
	}
	catch (const std::exception& e) {
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
	logger.set_level(LogLevel::VERBOSE);
	ow.begin(&ds);
	plugins.action(2, 0); // initialized
	logger.set_level(LogLevel::ERROR);
}
