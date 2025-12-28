#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"

extern DS2482 ds;
extern OwDevices ow;
extern SwitchHandler swHdl;

class SwTest : public ::testing::Test {
protected:
    void SetUp() override {
        ow.init();
		ow.begin(&ds);
		char buf[18];
		int pos = 3;
		uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0xcc };
		sprintf(buf, "%02X.", adr[0]);
		for (int j = 1; j < 8; j++) {
			sprintf(&buf[pos], "%02X", adr[j]);
			pos += 2;
		}
		ow.update_device(adr[2], buf);
		adr[2] = 2;
		pos = 3;
		sprintf(buf, "%02X.", adr[0]);
		for (int j = 1; j < 8; j++) {
			sprintf(&buf[pos], "%02X", adr[j]);
			pos += 2;
		}
		ow.update_device(adr[2], buf);
		ow.update_data();

		sw_tbl[0].src.sa.bus = 1;
		sw_tbl[0].src.sa.latch = 3;
		sw_tbl[0].src.sa.adr = 2;
		sw_tbl[0].src.sa.press = 0;
		sw_tbl[0].dst.da.pio = 0;
		sw_tbl[0].dst.da.bus = 2;
		sw_tbl[0].dst.da.adr = 2;
		sw_tbl[0].dst.da.type = TYPE_DS29X;
		cache.switches.push_back(sw_tbl[0]);
		swHdl.begin(&ds);
	}
};

#include <chrono>
using HrClock = std::chrono::high_resolution_clock;

TEST_F(SwTest, switches)
{
	HrClock::time_point tp;

	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0xcc };
	tp = HrClock::now();

	swHdl.dev_alarm(1, adr);
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(HrClock::now() - tp);

	logger.info(std::format("used {} ", duration));
	swHdl.dev_alarm(1, adr);
}