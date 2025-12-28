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

void log_time()
{

}

void load_json()
{
	string f = std::filesystem::current_path();
	f = f + "/test/data.json";
	try {
		ow.load(f.c_str());
	}
	catch (const std::exception& e) {
		printf("loading failed %s\n" , e.what());
	}
}

void segfault_handler(int signal)
{
	std::cerr << "Caught segmentation fault (signal " << signal << ")\n";
	std::exit(signal); // Gracefully terminate
}

TEST(main, LoadJson)
{
	string f = std::filesystem::current_path();
	f = f + "/test/data.json";
	try {
		ow.load(f.c_str());
		EXPECT_EQ(0, 0);
		// set log level back if changed by test
		logger.set_level(LogLevel::WARN);
	}
	catch (const std::exception& e) {
		printf("loading failed %s\n" , e.what());
	}
	ow.save(f);
	EXPECT_EQ(0, 0);
	ow.init();
}

int main(int argc, char* argv[])
{
	std::signal(SIGSEGV, segfault_handler);
	ow.begin(&ds);
	ow.set_mode(0x10);
	logger.set_level(LogLevel::WARN);
	logger.log( LogLevel::INFO, "Daemon started successfully.");
	logger.log( LogLevel::VERBOSE, "verbose mode.");
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS(); // This one line finds and runs every TEST() in the binary
}
