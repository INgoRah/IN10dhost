#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fuse3/fuse.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"

extern OwDevices ow;
extern DS2482 ds;

#if 0
class MockDS2482 : public DS2482 {
public:
    // MOCK_METHOD(Return, Name, (Arguments), (Qualifiers))
    MOCK_METHOD(bool, search, (uint8_t *newAddr, bool search_mode), ());
};

MockDS2482 mds;
#endif
extern void fs_init(fuse_operations* fs_ops);
static struct fuse_operations fs_ops = {};

class DevTest : public ::testing::Test {
protected:
    void SetUp() override {
		fs_init(&fs_ops);
        ow.init();
		ow.begin(&ds);
    }
};

/*
== Ch 0 ==
#1: 20 2 0 FD FF 66 77 34
#2: 28 5 0 FA FF 66 77 C6
#3: 29 8 0 F7 FF 66 77 2C
#4: 29 2 0 FD FF 66 77 F8
#5: 29 6 0 F9 FF 66 77 2A
#6: 29 1 0 FE FF 66 77 29
#7: 29 5 0 FA FF 66 77 FB
#8: 29 3 0 FC FF 66 77 40
8 devs found
== Ch 1 ==
#1: 28 7 1 F8 FE 66 77 C9
#2: 29 4 1 FB FE 66 77 25
#3: 29 C 1 F3 FE 66 77 98
#4: 29 2 1 FD FE 66 77 9E
#5: 29 1 1 FE FE 66 77 4F
#6: 29 5 1 FA FE 66 77 9D
#7: 29 3 1 FC FE 66 77 26
#8: 29 B 1 F4 FE 66 77 9B
#9: 29 7 1 F8 FE 66 77 F4
9 devs found
== Ch 2 ==
#1: 28 4 2 FB FD 66 77 B2
#2: 28 1 2 FE FD 66 77 D8
#3: 29 4 2 FB FD 66 77 8F
#4: 29 2 2 FD FD 66 77 34
#5: 29 6 2 F9 FD 66 77 E6
#6: 29 1 2 FE FD 66 77 E5
#7: 29 5 2 FA FD 66 77 37
#8: 29 3 2 FC FD 66 77 8C
#9: 29 7 2 F8 FD 66 77 5E
*/
TEST_F(DevTest, add_devices)
{
	char buf[18];
	int pos = 3;
	uint8_t  adr[8] = { 0x28, 0x5, 0x1, 0xFA, 0xFE, 0x66, 0x77, 0xff};
	// Note: the last is the CRC which will be updated to 0xA0
	sprintf(buf, "%02X.", adr[0]);
	for (int j = 1; j < 8; j++) {
		sprintf(&buf[pos], "%02X", adr[j]);
		pos += 2;
	}
	//printf("Adding device %s and checking...", buf);
	EXPECT_EQ(ow.find(1, 5, 0x28), nullptr);
	ow.update_device(1, buf);
	ow.update_data();
	EXPECT_NE(ow.find(1, 5, 0x28), nullptr);
	EXPECT_NE(ow.find(0x280501fafe6677a0), nullptr);
	EXPECT_NE(ow.find("28.0501FAFE6677A0"), nullptr);
	EXPECT_EQ(ow.find(1, 5), nullptr);
	// do not find this:
	EXPECT_EQ(ow.find(0, 5), nullptr);
	EXPECT_EQ(ow.find(0x290701F8FE6677F4), nullptr);
	EXPECT_EQ(ow.find(1, 7), nullptr);

	//printf("adding device 29.0200FDFF6677F8...");
	ow.update_device(0, "29.0200FDFF6677F8");
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_device(0, "20.0200F8FE6677F4");
	ow.update_data();
	EXPECT_NE(ow.find(0, 2), nullptr);
	EXPECT_NE(ow.find(0x290200FDFF6677F8), nullptr);
	EXPECT_NE(ow.find(1, 7), nullptr);
	EXPECT_NE(ow.find(0x290701F8FE6677F4), nullptr);

	// list all 3 devices
    std::vector<OwDev*>  devs = ow.list_devices(1);
	EXPECT_EQ(devs.size(), 2);
    devs = ow.list_devices(0);
	EXPECT_EQ(devs.size(), 2);
	// add again and check no duplicates
	ow.update_device(0, "29.0200FDFF6677F8");
	ow.update_data();
	devs = ow.list_devices(0);
	EXPECT_EQ(devs.size(), 2);
}

TEST_F(DevTest, Polling)
{
	char buf[8];
	int res;

	// add a device with poll interval of 5 seconds
	ow.update_device(0, "28.0501FAFE6677A0");
	res = fs_ops.read("/28.0501FAFE6677A0/poll", buf, 5, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "0");
	ow.poll();
	res = ow.poll();
	// no polling (no device)
	EXPECT_EQ(res, -1);

	sprintf(buf, "%d", 1);
	// initialize
	// @0
	ow.poll_time();
	usleep(500*1000);
	res = fs_ops.write("/28.0501FAFE6677A0/poll", buf, strlen(buf), 0, nullptr);
	res = fs_ops.read("/28.0501FAFE6677A0/poll", buf, 5, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "1");
	// let the device poll in between the seconds
	// @500 ms
	ow.poll();
	res = ow.poll_time();
	EXPECT_LE(res, 500);
	usleep(250*1000);
	// @750 ms
	// no polling device yet
	res = ow.poll_time();
	// remaining 250 not polling yet
	EXPECT_LE(res, 250);
	res = ow.poll();
	// device still needs 500 ms till polling
	EXPECT_EQ(res, 0);
	usleep(250*1000);
	res = ow.poll_time();
	// @1000 ms
	// seconds polling
	res = ow.poll();
	// TODO add a plugin action and check it is called or at least
	// returning 1
	//EXPECT_EQ(res, 1);
	// device should poll now
	usleep(500*1000);
	// @1500 ms
	// device should poll now
	res = ow.poll();
	EXPECT_EQ(res, 1);
	// now the remaining for the second
	res = ow.poll_time();
	EXPECT_LE(res, 500);
	usleep(500*1000);
	res = ow.poll_time();
	EXPECT_EQ(res, 0);
	ow.poll();
}

TEST_F(DevTest, ds2482)
{
	uint16_t crc = ds.crc16((const uint8_t*)"123456789", 9, 0);
	ds.check_crc16((const uint8_t*)"123456789", 9, (const uint8_t*)"\xB8\x66", 0);
	// printout for value
	//logger.error(std::format("crc: {}", crc));
	EXPECT_EQ(crc, 47933);
}