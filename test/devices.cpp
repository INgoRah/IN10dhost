#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fstream>
#include <vector>
#include <fuse3/fuse.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"
#include "ds2450.h"

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
	// wildcard type: matches on bus+id alone
	EXPECT_NE(ow.find(0, 2, 0xff), nullptr);

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

	// dump the recorded 1-Wire trace through the real FUSE read path
	// (matches the size fs_getattr advertises for /log/1wire.vcd)
	// buf size is AI generated
	std::vector<char> vcd_buf(218 + 1024 * 30);
	int vcd_res = fs_ops.read("/log/1wire.vcd", vcd_buf.data(), vcd_buf.size(), 0, nullptr);
	EXPECT_GT(vcd_res, 0);
	std::ofstream out("1wire.vcd", std::ios::binary);
	out.write(vcd_buf.data(), vcd_res);
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

	// dormant / simulation-only entry points that no device driver ever
	// reaches through its normal fs_read/fs_write path
	ds.resetDev();
	EXPECT_TRUE(ds.configureDev(0x01));
	ds.target_search(0x29);
	uint8_t adr[8] = {0};
	// simulated (USE_I2C off) search() always reports "no more devices"
	// right after issuing reset+write, never entering the real bit walk
	EXPECT_FALSE(ds.search(adr, true));
}

TEST_F(DevTest, ds2482_log_dump)
{
	// no buffer / zero size guard
	EXPECT_EQ(ds.log_dump(nullptr, 0), 0);
	EXPECT_EQ(ds.log_dump(nullptr, 16), 0);

	// buffer too small for even the first header line: exercises the
	// truncation branch inside the append_to_buf lambda
	char tiny[4];
	EXPECT_GT(ds.log_dump(tiny, sizeof(tiny)), 0);

	// channel 2 is never selected by any device added elsewhere in this
	// suite (they all live on bus 0/1), so its VCD identifier ('d') is
	// otherwise never exercised.
	// Sized generously (not just to fit this one entry): by the time
	// this test runs, every earlier test in the whole binary has been
	// pushing entries into the same global, ring-buffer-backed log, and
	// log_dump() writes oldest-first, so a buffer only sized for this
	// one entry would get truncated by everything logged before it.
	ds.selectChannel(2);
	ds.reset();
	std::vector<char> buf(256 * 1024);
	int n = ds.log_dump(buf.data(), buf.size());
	ASSERT_GT(n, 0);
	EXPECT_NE(std::string(buf.data(), n).find(" d\n"), std::string::npos);
}

TEST_F(DevTest, ds2450)
{
	// boundary/API-only checks on a standalone (not bus-bound) instance
	ds2450 raw;
	EXPECT_EQ(raw.adc_read(5, 0), -1);       // out-of-range channel
	EXPECT_FLOAT_EQ(raw.adc_get(0), 0.0f);
	EXPECT_FLOAT_EQ(raw.adc_get(1), 0.0f);
	EXPECT_FLOAT_EQ(raw.adc_get(2), 0.0f);
	EXPECT_FLOAT_EQ(raw.adc_get(3), 0.0f);
	EXPECT_FLOAT_EQ(raw.adc_get(9), -1.0f);  // out-of-range channel

	ow.update_device(0, "20.0300F8FE6677F5");
	ow.update_data();
	// the trailing ROM byte is a CRC that OwDev::update() recomputes on
	// load, so the path actually served by FUSE isn't the literal string
	// above; look the device up and use its corrected rom for paths
	ds2450* dev = (ds2450*)ow.find(0, 3, 0x20);
	ASSERT_NE(dev, nullptr);
	std::string base = "/" + dev->rom;
	char buf2[32];
	int res;

	// buffer too small for a "x.xx" voltage reading
	res = fs_ops.read((base + "/volt.A").c_str(), buf2, 3, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// path not handled by ds2450 itself falls back to the base OwDev
	// read/write implementation
	res = fs_ops.write((base + "/name").c_str(), (char*)"adc", 4, 0, nullptr);
	EXPECT_GT(res, 0);
	res = fs_ops.read((base + "/name").c_str(), buf2, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf2, "adc");

	// drive the device-level poll() override: no interval set yet ->
	// delegates straight to OwDev::poll()'s "no polling" (-1) path
	EXPECT_EQ(dev->poll(), -1);

	// set a 1s interval and let it elapse so OwDev::poll() reports 1,
	// driving ds2450::poll()'s own ADC-read branch
	res = fs_ops.write((base + "/poll").c_str(), (char*)"1", 1, 0, nullptr);
	EXPECT_GT(res, 0);
	usleep(1100 * 1000);
	EXPECT_EQ(dev->poll(), 1);

	// cached read (flag 2), only reachable by calling adc_read()
	// directly since fs_read_volt() never passes it through; dat[]
	// was just populated by the poll() above
	EXPECT_EQ(dev->adc_read(0, 2), 0);
}

// every other config-loading test in this suite uses a ds2408-only
// fixture (test/plugin.json); this drives make_device_from_json()'s
// other three device types, only reachable by actually loading them
// from a config file rather than the in-memory update_device() path
TEST_F(DevTest, LoadAllDeviceTypesFromJson)
{
	const char* path = "test_all_device_types.json";
	std::ofstream(path) << R"({
		"version": 1,
		"bus_count": 4,
		"mode": 0,
		"busses": [
			{"id": 0, "dev_count": 0}, {"id": 1, "dev_count": 0},
			{"id": 2, "dev_count": 0}, {"id": 3, "dev_count": 0}
		],
		"devices": [
			{"type": "ds2408", "bus": 0, "rom": "29.0400FDFF6677FA", "id": 0, "name": ""},
			{"type": "ds1820", "bus": 0, "rom": "28.0501FAFE6677A0", "id": 0, "name": ""},
			{"type": "ds2450", "bus": 0, "rom": "20.0300F8FE6677F5", "id": 0, "name": ""},
			{"type": "ard_i2c", "bus": 0, "rom": "AD.0900F8FF6677E2", "id": 0, "name": ""}
		]
	})";

	// note: deliberately NOT reusing 29.0200FDFF6677F8 (rom_code
	// 0x290200FDFF6677F8) here - the "example" test plugin's action()
	// hardcodes that exact rom_code and calls pio_set(1) on whatever
	// device holds it whenever plugins.action(INITIALIZED) fires,
	// which ow.load() does at the end of every call
	ow.load(path);
	EXPECT_NE(ow.find(0, 4, 0x29), nullptr);
	EXPECT_NE(ow.find(0, 5, 0x28), nullptr);
	EXPECT_NE(ow.find(0, 3, 0x20), nullptr);
	EXPECT_NE(ow.find(0, 9, 0xAD), nullptr);
	std::remove(path);
}

TEST_F(DevTest, LoadUnknownDeviceTypeThrows)
{
	const char* path = "test_unknown_device_type.json";
	std::ofstream(path) << R"({
		"version": 1,
		"bus_count": 4,
		"mode": 0,
		"busses": [
			{"id": 0, "dev_count": 0}, {"id": 1, "dev_count": 0},
			{"id": 2, "dev_count": 0}, {"id": 3, "dev_count": 0}
		],
		"devices": [
			{"type": "not_a_real_device", "bus": 0, "rom": "29.0200FDFF6677F8", "id": 0, "name": ""}
		]
	})";

	EXPECT_THROW(ow.load(path), std::runtime_error);
	std::remove(path);
}