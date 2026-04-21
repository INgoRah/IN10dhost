#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <csignal>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "logger.h"
#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"
#include "switch_handler.h"
#include "ard_i2c.h"

class MockOwDev : public DS2482 {
public:
	MockOwDev(const std::string& i2c_dev, int address) { (void)i2c_dev; (void)address; };
	MOCK_METHOD(int, read, (), ());
	MOCK_METHOD(int, write, (const uint8_t *buf, uint16_t count), ());
};

Logger logger;
DS2482 ds("/dev/i2c-0", 0x18);
OwDevices ow;
SwitchHandler swHdl (&ow);
Ard_i2c arduino;

extern int test_devices();
extern int test_fs();

void segfault_handler(int signal)
{
	std::cerr << "Caught segmentation fault (signal " << signal << ")\n";
	std::exit(signal); // Gracefully terminate
}

TEST(main, LoadJson)
{
	bool ok = false;
	string f = std::filesystem::current_path();
	LogLevel lvl = logger.get_level();
	ow.begin(&ds);
	ow.set_mode(0x10);
	try {
		ow.load("invalid.json");
	}
	catch (const std::exception& e) {
		// loading failed as expected
		ok = true;
	}
	EXPECT_EQ(ok, true);

	f = f + "/test/data.json";
	try {
		ow.load(f.c_str());
		ok = true;
	}
	catch (const std::exception& e) {
		logger.error(std::format("loading failed %s\n", e.what()));
		ok = false;
	}
	EXPECT_EQ(ok, true);
	// set log level back if changed by test
	logger.set_level(lvl);
	ow.save(f);
	// todo check if file exists
	EXPECT_EQ(0, 0);
	ow.init();
}

TEST(main, Logging)
{
	LogLevel lvl = logger.get_level();
	// full coverage in logger
	logger.set_level(LogLevel::VERBOSE);
	logger.error("test error meesage");
	logger.warn("test warn meesage");
	logger.debug("test debug message");
	logger.info("test info message");
	logger.verbose("test verbose message");
	logger.log(LogLevel::NONE, "test unkown meesage");
	logger.set_level(lvl);
}

int main(int argc, char* argv[])
{
	std::signal(SIGSEGV, segfault_handler);
	logger.set_level(LogLevel::ERROR);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS(); // This one line finds and runs every TEST() in the binary
}
