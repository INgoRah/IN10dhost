#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fuse3/fuse.h>

#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"

extern DS2482 ds;
extern OwDevices ow;
extern SwitchHandler swHdl;
// defined in src/switch_handler.cpp
extern void to_json(json& j, const _sw_tbl& b);
extern void from_json(const json& j, _sw_tbl& b);
extern void from_json(const json& j, Config& c);

extern void fs_init(fuse_operations* fs_ops);
static struct fuse_operations fs_ops = {};

static int filler(void *buf, const char *name,
				const struct stat *stbuf, off_t off,
				enum fuse_fill_dir_flags flags)
{
	(void)buf;
	(void)name;
	(void)stbuf;
	(void)off;
	(void)flags;
	return 0;
}

class SwTest : public ::testing::Test {
protected:
	void SetUp() override {
		logger.set_level(LogLevel::ERROR);
		fs_init(&fs_ops);
		ow.init();
		ow.begin();
		char buf[18];
		int pos = 3;
		uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0xcc };
		sprintf(buf, "%02X.", adr[0]);
		for (int j = 1; j < 8; j++) {
			sprintf(&buf[pos], "%02X", adr[j]);
			pos += 2;
		}
		// 1.2 crc = 9a
		ow.update_device(adr[2], buf);
		adr[2] = 2;
		pos = 3;
		sprintf(buf, "%02X.", adr[0]);
		for (int j = 1; j < 8; j++) {
			sprintf(&buf[pos], "%02X", adr[j]);
			pos += 2;
		}
		// 2.2 crc =d4
		ow.update_device(adr[2], buf);
		ow.update_data();
		ow.begin();
		swHdl.begin(&ds);
	}
};

#include <chrono>
using HrClock = std::chrono::high_resolution_clock;

TEST_F(SwTest, switching)
{
	char buf[32];
	int res;
	bool ret;
	HrClock::time_point tp;

	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	tp = HrClock::now();
	buf[0] = '3';
	buf[1] = '3';
	buf[2] = '\0';
	res = fs_ops.write("/29.0202abbd6677d4/pin.0/func", buf, 3, 0, nullptr);
	res = fs_ops.write("/29.0202abbd6677d4/pin.1/func", buf, 3, 0, nullptr);
	buf[0] = '0';
	buf[1] = 0;
	res = fs_ops.write("/29.0202abbd6677d4/PIO.0", buf, 2, 0, nullptr);
	strcpy(buf, "1 2 3 2 2 0");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_GT(res, 0);

	ds2408* dev = (ds2408*)ow.find(0x290201ABBD66779A);
	dev->data[PIO_LS] = 0xde;
	dev->data[PIO_LATCH] = 0x04;
	dev->data[PIO_TIME] = 0xff;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, true);
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(HrClock::now() - tp);
	logger.info(std::format("used {} ", duration));
	res = fs_ops.read("/29.0202abbd6677d4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_STREQ(buf, "1");
	// the alarm read reset the latch on the device (and so in data[]),
	// a second press sets it again
	EXPECT_EQ(dev->data[PIO_LATCH], 0);
	dev->data[PIO_LATCH] = 0x04;
	ret = swHdl.dev_alarm(1, adr);
	res = fs_ops.read("/29.0202abbd6677d4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	// watchdog
	dev->data[STAT] = 0x88;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, true);
	// check wrong latch content
	dev->data[STAT] = 0x0;
	dev->data[PIO_LATCH] = 0xff;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, false);
	// check different time values
	dev->data[PIO_LATCH] = 0x02;
	dev->data[PIO_TIME] = 0x05;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, true);
	dev->data[PIO_TIME] = 0x0;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, true);
	dev->data[PIO_TIME] = 0x25;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, true);

	// check dev alarm with no sw entry
	adr[1] = 0x03;
	ret = swHdl.dev_alarm(1, adr);
	EXPECT_EQ(ret, false);

	// actor_handle() against a bus/address with no registered device
	union pio dst;
	dst.data = 0;
	dst.da.bus = 3;
	dst.da.adr = 0x7f;
	EXPECT_EQ(swHdl.actor_handle(dst, TOGGLE), false);

	// alarmHandler() now lives on OwDevices; it is a no-op under
	// USE_I2C=OFF, never called from anywhere else in this build
	EXPECT_EQ(ow.alarmHandler(0), false);
}

TEST_F(SwTest, FsSwitches)
{
	char buf[1024];
	int res;
	struct stat st;

	res = fs_ops.readdir("/switches", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/switches", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	// "add"/"del" are write-only (no read handler) but must still open
	// successfully - a real client always open()s before write()ing,
	// and a write never even reaches fs_write() if open() refuses it
	res = fs_ops.open("/switches/add", nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.open("/switches/del", nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.open("/switches/list", nullptr);
	EXPECT_EQ(res, 0);
	// error checking, missing number
	strcpy(buf, "1 2 3 1 2");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_NE(res, 0);
	// error checking, no number
	strcpy(buf, "1 2 3 1 A B");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_NE(res, 0);

	strcpy(buf, "1 2 3 1 2 0");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	logger.verbose(std::format("{} returned from add", res));
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	logger.verbose(std::format("{} returned from del", res));
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, 0);

	// same add/del round-trip, but using the more compact "bus.adr.latch
	// bus.adr.pio" format instead of six space-separated numbers
	strcpy(buf, "2.3.4 1.3.1");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, 0);

	// arrow-separated format: "bus.adr.latch -> bus.adr.pio"
	strcpy(buf, "3.4.5 -> 2.4.2");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, 0);

	// anything past the 6th number is ignored rather than rejected;
	// deleting with the same values but no trailing junk must still
	// match the switch that was added with it
	strcpy(buf, "4.5.6 3.5.3 ignored junk");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	strcpy(buf, "4.5.6 3.5.3");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, 0);

	// adding the same switch twice must not create a duplicate entry
	strcpy(buf, "5.6.7 4.6.4");
	buf [strlen(buf)] = 0;
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	char listbuf[1024];
	int len1 = fs_ops.read("/switches/list", listbuf, sizeof(listbuf), 0, nullptr);
	EXPECT_GT(len1, 0);
	res = fs_ops.write("/switches/add", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	int len2 = fs_ops.read("/switches/list", listbuf, sizeof(listbuf), 0, nullptr);
	EXPECT_EQ(len1, len2);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, strlen(buf) + 1);
	res = fs_ops.write("/switches/del", buf, strlen(buf) + 1, 0, nullptr);
	EXPECT_EQ(res, 0);

	res = fs_ops.getattr("/switches/list", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.read("/switches/list", buf, 1024, 0, nullptr);
	EXPECT_GT(res, 0);
	res = fs_ops.getattr("/switches/na", &st, nullptr);
	EXPECT_NE(res, 0);

	// a switches path fs_read() doesn't recognize (not "list")
	res = fs_ops.read("/switches/add", buf, 1024, 0, nullptr);
	EXPECT_EQ(res, 0);
}

TEST_F(SwTest, SwitchesConfig)
{
	ow.save("test_switches.json");
	ow.load("test_switches.json");
	logger.set_level(LogLevel::ERROR);
}

TEST(LLSwTest, ll_funcs) {
	swHdl.cur_latch = 0x01;
	EXPECT_EQ(swHdl.bitnumber(), 1);
	swHdl.cur_latch = 0x08;
	EXPECT_EQ(swHdl.bitnumber(), 4);
	swHdl.cur_latch = 0x0;
	EXPECT_EQ(swHdl.bitnumber(), 0xff);
}
TEST_F(SwTest, TimedSwitch)
{
	char buf[1024];
	int res;
	// source device 1.2, target device 2.2
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ASSERT_NE(src, nullptr);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 8; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	// a press on src latch n (1..8)
	auto press = [&](int latch) {
		src->data[STAT] = 0;
		src->data[PIO_TIME] = 0xff;
		src->data[PIO_LATCH] = 1 << (latch - 1);
		return swHdl.dev_alarm(1, adr);
	};
	auto pio1 = [&]() {
		buf[0] = '\0';
		fs_ops.read("/29.0202abbd6677d4/PIO.1", buf, 2, 0, nullptr);
		return std::string(buf);
	};
	union pio target;
	target.data = 0;
	target.da.bus = 2;
	target.da.adr = 2;
	target.da.pio = 1;

	// latch 5: 30s, a press restarts the timer
	// latch 6: 30s, a press switches off
	const char* add = "1.2.5 2.2.1 30 0\n1.2.6 2.2.1 30 1";
	res = fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);
	EXPECT_EQ(res, (int)strlen(add) + 1);
	res = fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
	ASSERT_GT(res, 0);
	EXPECT_NE(std::string(buf).find("1.2.5 -> 2.2.1 30 0: "), std::string::npos);
	EXPECT_NE(std::string(buf).find("timed 30s, press restarts"), std::string::npos);
	EXPECT_NE(std::string(buf).find("timed 30s, press off"), std::string::npos);

	// stored with the config and restored from it
	bool seen = false;
	for (const auto& sw : cache.switches) {
		if (sw.src.sa.latch != 6 || sw.dst.data != target.data)
			continue;
		json j = sw;
		EXPECT_EQ(j["secs"], 30);
		EXPECT_EQ(j["on_press"], SW_ON_PRESS_OFF);
		_sw_tbl back = j.get<_sw_tbl>();
		EXPECT_EQ(back.secs, 30);
		EXPECT_EQ(back.on_press, SW_ON_PRESS_OFF);
		seen = true;
	}
	EXPECT_TRUE(seen);

	fs_ops.write("/29.0202abbd6677d4/PIO.1", "0", 2, 0, nullptr);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));

	// retrigger: on, a second press keeps it on and counts from then
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_TRUE(swHdl.timer_running(target));
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(25)), 0);
	EXPECT_EQ(pio1(), "1");
	// expired: off, timer gone
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 1);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));

	// off on press: on, the second press switches off and stops it
	press(6);
	EXPECT_EQ(pio1(), "1");
	EXPECT_TRUE(swHdl.timer_running(target));
	press(6);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 0);

	// both share the target's timer: on by 5, off by 6
	press(5);
	EXPECT_EQ(pio1(), "1");
	press(6);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));

	// already on (here from the file system): a timed press starts no
	// timer - a "restarts" one leaves it on, an "off" one switches off
	fs_ops.write("/29.0202abbd6677d4/PIO.1", "1", 2, 0, nullptr);
	EXPECT_EQ(pio1(), "1");
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_FALSE(swHdl.timer_running(target));
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 0);
	EXPECT_EQ(pio1(), "1");
	press(6);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));

	// the same when another (plain) button switched it on
	const char* plain = "1.2.4 2.2.1";
	fs_ops.write("/switches/add", plain, strlen(plain) + 1, 0, nullptr);
	fs_ops.write("/29.0202abbd6677d4/PIO.1", "0", 2, 0, nullptr);
	press(4);
	EXPECT_EQ(pio1(), "1");
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_FALSE(swHdl.timer_running(target));
	press(6);
	EXPECT_EQ(pio1(), "0");
	press(4);
	EXPECT_EQ(pio1(), "1");
	press(4);
	EXPECT_EQ(pio1(), "0");

	// the plain button switches off while the timer runs: timer done,
	// the next timed press switches on again with a new one
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_TRUE(swHdl.timer_running(target));
	press(4);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));
	press(5);
	EXPECT_EQ(pio1(), "1");
	EXPECT_TRUE(swHdl.timer_running(target));

	// a plain button switching on while an old timer is still pending
	// (here: switched off through the file system meanwhile) ends that
	// timer, the light stays on for good
	press(5);
	EXPECT_TRUE(swHdl.timer_running(target));
	fs_ops.write("/29.0202abbd6677d4/PIO.1", "0", 2, 0, nullptr);
	press(4);
	EXPECT_EQ(pio1(), "1");
	EXPECT_FALSE(swHdl.timer_running(target));
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 0);
	EXPECT_EQ(pio1(), "1");
	press(4);
	EXPECT_EQ(pio1(), "0");

	// timed buttons share the light's timer: another one restarts it
	// with its own time, or switches off and stops it
	const char* longer = "1.2.7 2.2.1 90 0";
	fs_ops.write("/switches/add", longer, strlen(longer) + 1, 0, nullptr);
	press(5);
	EXPECT_EQ(pio1(), "1");
	press(7);
	EXPECT_EQ(pio1(), "1");
	EXPECT_GT(swHdl.timer_remaining(target), 60);
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 0);
	EXPECT_EQ(pio1(), "1");
	press(6);
	EXPECT_EQ(pio1(), "0");
	EXPECT_FALSE(swHdl.timer_running(target));
	fs_ops.write("/switches/del", longer, strlen(longer) + 1, 0, nullptr);

	// switched off from the file system while the timer runs: that
	// timer is stale, the next timed press switches on and starts anew
	fs_ops.write("/29.0202abbd6677d4/PIO.1", "0", 2, 0, nullptr);
	press(6);
	EXPECT_EQ(pio1(), "1");
	EXPECT_TRUE(swHdl.timer_running(target));
	EXPECT_EQ(swHdl.timer_poll(HrClock::now() + std::chrono::seconds(31)), 1);
	EXPECT_EQ(pio1(), "0");
	fs_ops.write("/switches/del", plain, strlen(plain) + 1, 0, nullptr);

	// adding again updates the timing, no duplicate
	const char* upd = "1.2.5 2.2.1 60 1";
	fs_ops.write("/switches/add", upd, strlen(upd) + 1, 0, nullptr);
	fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
	EXPECT_NE(std::string(buf).find("timed 60s, press off"), std::string::npos);
	EXPECT_EQ(std::string(buf).find("timed 30s, press restarts"), std::string::npos);

	// out of range timing is rejected
	const char* bad[] = { "1.2.7 2.2.1 70000", "1.2.7 2.2.1 30 2" };
	for (const char* b : bad) {
		fs_ops.write("/switches/add", b, strlen(b) + 1, 0, nullptr);
		fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
		EXPECT_EQ(std::string(buf).find("1.2.7 "), std::string::npos) << b;
	}

	const char* del = "1.2.5 2.2.1\n1.2.6 2.2.1";
	res = fs_ops.write("/switches/del", del, strlen(del) + 1, 0, nullptr);
	EXPECT_EQ(res, (int)strlen(del) + 1);
}

TEST_F(SwTest, SwitchList)
{
	char buf[2048];
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 8; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	fs_ops.write("/29.0202abbd6677d4/PIO.2", "0", 2, 0, nullptr);
	auto list = [&]() {
		struct stat st;
		// the size getattr reports must cover the whole list, it grows
		// with the names
		fs_ops.getattr("/switches/list", &st, nullptr);
		int n = fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
		EXPECT_EQ(n, st.st_size);
		return std::string(buf);
	};
	auto line_of = [&](const std::string& all, const std::string& start) {
		size_t b = all.find(start);
		if (b == std::string::npos)
			return std::string();
		return all.substr(b, all.find('\n', b) - b);
	};

	const char* add = "1.2.6 2.2.2\n1.2.17 2.2.2 45 1\n3.9.1 3.9.0";
	fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);

	// without names: the roms and PIO.n
	std::string all = list();
	EXPECT_EQ(line_of(all, "1.2.6 -> 2.2.2"),
		"1.2.6 -> 2.2.2: 29.0201ABBD66779A, PIO.5 (latch 6) -> 29.0202ABBD6677D4, PIO.2");
	// long press as added (17), timing
	EXPECT_EQ(line_of(all, "1.2.17 -> 2.2.2"),
		"1.2.17 -> 2.2.2 45 1: 29.0201ABBD66779A, PIO.6 (latch 7, long) -> 29.0202ABBD6677D4, PIO.2, timed 45s, press off");
	// no such devices
	EXPECT_EQ(line_of(all, "3.9.1 -> 3.9.0"),
		"3.9.1 -> 3.9.0: unknown device 3.9 (latch 1) -> unknown device 3.9");

	// with names
	fs_ops.write("/29.0201ABBD66779A/name", "Hallway\n", 8, 0, nullptr);
	fs_ops.write("/29.0201ABBD66779A/pin.6/name", "Stairs button\n", 14, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/name", "Light board\n", 12, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/pin.2/name", "Stairs\n", 7, 0, nullptr);
	all = list();
	EXPECT_EQ(line_of(all, "1.2.17 -> 2.2.2"),
		"1.2.17 -> 2.2.2 45 1: Hallway, Stairs button (latch 7, long) -> Light board, Stairs, timed 45s, press off");

	// a running timer shows when it switches off
	src->data[STAT] = 0;
	src->data[PIO_TIME] = 20;	// long press
	src->data[PIO_LATCH] = 1 << 6;
	swHdl.dev_alarm(1, adr);
	all = list();
	EXPECT_NE(line_of(all, "1.2.17 -> 2.2.2").find(", press off, off in 4"), std::string::npos)
		<< line_of(all, "1.2.17 -> 2.2.2");

	// the start of a line is what add/del take
	std::string del = "1.2.17 -> 2.2.2\n1.2.6 -> 2.2.2\n3.9.1 -> 3.9.0";
	EXPECT_EQ(fs_ops.write("/switches/del", del.c_str(), del.size() + 1, 0, nullptr), (int)del.size() + 1);
	all = list();
	EXPECT_EQ(all.find("2.2.2"), std::string::npos);

	// back to the defaults for other tests
	fs_ops.write("/29.0201ABBD66779A/name", "\n", 1, 0, nullptr);
	fs_ops.write("/29.0201ABBD66779A/pin.6/name", "\n", 1, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/name", "\n", 1, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/pin.2/name", "\n", 1, 0, nullptr);
	swHdl.timer_poll(HrClock::now() + std::chrono::seconds(60));
}

// Target 0.0.0 switches every light off, whatever device it is on
TEST_F(SwTest, AllLightsOff)
{
	char buf[2048];
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 4; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	// 0, 1 lights, 2 a socket
	fs_ops.write("/29.0202ABBD6677D4/pin.0/role", "light", 5, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/pin.1/role", "light", 5, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/pin.2/role", "socket", 6, 0, nullptr);
	for (int i = 0; i < 3; i++)
		dst->pin_switch(i, ON);
	ASSERT_TRUE(dst->pin_is_on(0));
	ASSERT_TRUE(dst->pin_is_on(1));
	// a light with its off timer running
	union pio light1 = {};
	light1.da.bus = 2;
	light1.da.adr = 2;
	light1.da.pio = 1;
	swHdl.timer_start(light1, 30);

	// timing makes no sense here and is dropped
	const char* add = "1.2.3 0.0.0 30 1";
	fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);
	fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
	EXPECT_NE(std::string(buf).find("1.2.3 -> 0.0.0: 29.0201ABBD66779A, PIO.2 (latch 3) -> all lights off\n"),
		std::string::npos) << buf;

	src->data[STAT] = 0;
	src->data[PIO_TIME] = 0xff;
	src->data[PIO_LATCH] = 1 << 2;
	EXPECT_TRUE(swHdl.dev_alarm(1, adr));
	EXPECT_FALSE(dst->pin_is_on(0));
	EXPECT_FALSE(dst->pin_is_on(1));
	EXPECT_TRUE(dst->pin_is_on(2));
	EXPECT_FALSE(swHdl.timer_running(light1));

	// pressed again it stays off, nothing toggles
	src->data[PIO_LATCH] = 1 << 2;
	swHdl.dev_alarm(1, adr);
	EXPECT_FALSE(dst->pin_is_on(0));
	EXPECT_FALSE(dst->pin_is_on(1));

	const char* del = "1.2.3 0.0.0";
	EXPECT_EQ(fs_ops.write("/switches/del", del, strlen(del) + 1, 0, nullptr), (int)strlen(del) + 1);
	for (int i = 0; i < 3; i++)
		fs_ops.write(("/29.0202ABBD6677D4/pin." + std::to_string(i) + "/role").c_str(), "", 0, 0, nullptr);
	dst->pin_switch(2, OFF);
}

// Masked switches: their presses are ignored, one or all of them
TEST_F(SwTest, DisableSwitches)
{
	char buf[2048];
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 4; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	dst->pin_switch(1, OFF);
	dst->pin_switch(2, OFF);
	auto press = [&](int latch) {
		src->data[STAT] = 0;
		src->data[PIO_TIME] = 0xff;
		src->data[PIO_LATCH] = 1 << (latch - 1);
		swHdl.dev_alarm(1, adr);
	};
	auto list = [&]() {
		int n = fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
		return std::string(buf, n > 0 ? n : 0);
	};

	const char* add = "1.2.4 2.2.1\n1.2.5 2.2.2";
	fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);

	// one of them: its presses do nothing, the other still switches
	const char* one = "1.2.4 -> 2.2.1\n";
	EXPECT_EQ(fs_ops.write("/switches/disable", one, strlen(one), 0, nullptr), (int)strlen(one));
	EXPECT_NE(list().find("1.2.4 -> 2.2.1: 29.0201ABBD66779A, PIO.3 (latch 4) -> 29.0202ABBD6677D4, PIO.1, disabled\n"),
		std::string::npos) << buf;
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));
	press(5);
	EXPECT_TRUE(dst->pin_is_on(2));
	// adding it again keeps it masked
	fs_ops.write("/switches/add", "1.2.4 2.2.1", 12, 0, nullptr);
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));
	// saved with the switch and taken back from the config
	auto find = [&](int latch) -> const _sw_tbl* {
		for (const auto& sw : cache.switches)
			if (sw.src.sa.bus == 1 && sw.src.sa.adr == 2 && sw.src.sa.latch == latch &&
			    sw.src.sa.press == 0)
				return &sw;
		return nullptr;
	};
	ASSERT_NE(find(4), nullptr);
	ASSERT_NE(find(5), nullptr);
	json j = *find(4);
	EXPECT_EQ(j["disabled"], true);
	EXPECT_EQ(json(*find(5))["disabled"], false);
	_sw_tbl back;
	from_json(j, back);
	EXPECT_TRUE(back.disabled);
	// older configs: enabled
	j.erase("disabled");
	_sw_tbl old;
	from_json(j, old);
	EXPECT_FALSE(old.disabled);
	EXPECT_EQ(fs_ops.write("/switches/enable", one, strlen(one), 0, nullptr), (int)strlen(one));
	EXPECT_EQ(list().find("disabled"), std::string::npos) << buf;
	press(4);
	EXPECT_TRUE(dst->pin_is_on(1));

	// all of them
	EXPECT_EQ(fs_ops.write("/switches/disable", "all\n", 4, 0, nullptr), 4);
	EXPECT_NE(list().find("switches, dark, all disabled\n"), std::string::npos) << buf;
	press(4);
	press(5);
	EXPECT_TRUE(dst->pin_is_on(1));
	EXPECT_TRUE(dst->pin_is_on(2));
	// saved with the config, restored when it is loaded
	{
		namespace fsys = std::filesystem;
		fsys::path file = fsys::temp_directory_path() / "in10dfs_disable_test.json";
		ASSERT_EQ(ow.save(file.string()), 0);
		std::ifstream in(file);
		json cfg = json::parse(in);
		fsys::remove(file);
		EXPECT_EQ(cfg["switches_disabled"], true);
		swHdl.all_disabled_set(false);
		Config loaded;
		from_json(cfg, loaded);
		EXPECT_TRUE(swHdl.all_disabled());
		// older configs: enabled
		cfg.erase("switches_disabled");
		from_json(cfg, loaded);
		EXPECT_FALSE(swHdl.all_disabled());
		swHdl.all_disabled_set(true);
	}
	EXPECT_EQ(fs_ops.write("/switches/enable", "all", 3, 0, nullptr), 3);
	press(5);
	EXPECT_FALSE(dst->pin_is_on(2));

	// not in the table, not a switch line
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	EXPECT_EQ(fs_ops.write("/switches/disable", "1.2.6 -> 2.2.3", 14, 0, nullptr), -ENOENT);
	EXPECT_EQ(fs_ops.write("/switches/disable", "everything", 10, 0, nullptr), -EINVAL);
	logger.set_level(lvl);

	const char* del = "1.2.4 2.2.1\n1.2.5 2.2.2";
	fs_ops.write("/switches/del", del, strlen(del) + 1, 0, nullptr);
	dst->pin_switch(1, OFF);
}

// What ACT_ALARM_AFTER hands the plugins: for a ds2408 the latches of
// the alarm's register read and the sensed levels, see scary-door.js
TEST_F(SwTest, AlarmEvent)
{
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ASSERT_NE(src, nullptr);

	// latch 6 (PIO 5) pressed, PIO 5 and 0 read high
	src->data[STAT] = 0;
	src->data[PIO_TIME] = 0xff;
	src->data[PIO_LATCH] = 1 << 5;
	src->data[PIO_LS] = (1 << 5) | 1;
	swHdl.dev_alarm(1, adr);
	EXPECT_EQ(src->get_last_latch(), 1 << 5);

	json data = ow.alarm_event(1, adr);
	EXPECT_EQ(data["bus"], 1);
	EXPECT_EQ(data["rom"], "29.0201ABBD66779A");
	EXPECT_EQ(data["latch"], 1 << 5);
	EXPECT_EQ(data["sensed"], src->data[PIO_LS]);

	// not a ds2408 on that bus and address: only bus and rom
	adr[1] = 0x3f;
	data = ow.alarm_event(1, adr);
	EXPECT_EQ(data["rom"], "29.3F01ABBD66779A");
	EXPECT_FALSE(data.contains("latch"));
	EXPECT_FALSE(data.contains("sensed"));
	uint8_t temp[] = { 0x28, 0x05, 0x01, 0xfa, 0xfe, 0x66, 0x77, 0xa0 };
	data = ow.alarm_event(1, temp);
	EXPECT_EQ(data["rom"], "28.0501FAFE6677A0");
	EXPECT_FALSE(data.contains("latch"));
	src->data[PIO_LS] = 0;
}

// switchHandle() takes one latch bit per call, as dev_alarm() loops
// while there is one; called with none left it warns and does nothing
// (bitnumber() finds no bit)
TEST_F(SwTest, SwitchHandleNoLatch)
{
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	dst->cfg[CFG_PIN_ID + 1] = CFG_OUT_LOW;
	dst->pin_switch(1, OFF);
	fs_ops.write("/switches/add", "1.2.4 2.2.1", 12, 0, nullptr);

	// an alarm uses up all its latches
	src->data[STAT] = 0;
	src->data[PIO_TIME] = 0xff;
	src->data[PIO_LATCH] = 1 << 3;
	swHdl.dev_alarm(1, adr);
	EXPECT_TRUE(dst->pin_is_on(1));

	uint64_t warned = logger.warnings().count;
	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	EXPECT_FALSE(swHdl.switchHandle(1, 2));
	logger.set_level(lvl);
	EXPECT_EQ(logger.warnings().count, warned + 1);
	EXPECT_NE(logger.warnings().last.find("invalid latch data 1.2"), std::string::npos)
		<< logger.warnings().last;
	EXPECT_TRUE(dst->pin_is_on(1));

	fs_ops.write("/switches/del", "1.2.4 2.2.1", 12, 0, nullptr);
	dst->pin_switch(1, OFF);
}

static union pio dst_pio(int bus, int adr, int pio)
{
	union pio p = {};

	p.da.bus = bus;
	p.da.adr = adr;
	p.da.pio = pio;
	return p;
}

// "dark": switches the target on only while the astro flag says dark,
// off always
TEST_F(SwTest, DarkSwitches)
{
	char buf[2048];
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 4; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	dst->pin_switch(1, OFF);
	dst->pin_switch(2, OFF);
	auto press = [&](int latch) {
		src->data[STAT] = 0;
		src->data[PIO_TIME] = 0xff;
		src->data[PIO_LATCH] = 1 << (latch - 1);
		swHdl.dev_alarm(1, adr);
	};
	auto list = [&]() {
		int n = fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
		return std::string(buf, n > 0 ? n : 0);
	};
	auto dark = [&]() {
		int n = fs_ops.read("/switches/dark", buf, sizeof(buf), 0, nullptr);
		return std::string(buf, n > 0 ? n : 0);
	};

	// dark until told otherwise
	EXPECT_EQ(dark(), "1\n");
	const char* add = "1.2.4 2.2.1 dark\n1.2.5 -> 2.2.2 30 1 dark\n1.2.6 0.0.0 dark";
	fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);
	std::string all = list();
	EXPECT_NE(all.find("switches, dark\n"), std::string::npos) << all;
	EXPECT_NE(all.find("1.2.4 -> 2.2.1 dark: 29.0201ABBD66779A, PIO.3 (latch 4) -> 29.0202ABBD6677D4, PIO.1, only when dark\n"),
		std::string::npos) << all;
	EXPECT_NE(all.find("1.2.5 -> 2.2.2 30 1 dark: "), std::string::npos) << all;
	// all lights off takes no condition
	EXPECT_NE(all.find("1.2.6 -> 0.0.0: "), std::string::npos) << all;
	// saved with the switch, taken back from the config
	auto find = [&](int latch, int pio) -> const _sw_tbl* {
		for (const auto& sw : cache.switches)
			if (sw.src.sa.bus == 1 && sw.src.sa.adr == 2 && sw.src.sa.latch == latch &&
			    sw.src.sa.press == 0 && sw.dst.da.pio == pio)
				return &sw;
		return nullptr;
	};
	ASSERT_NE(find(4, 1), nullptr);
	ASSERT_NE(find(6, 0), nullptr);
	json j = *find(6, 0);
	EXPECT_EQ(j["cond"], SW_COND_NONE);
	j = *find(4, 1);
	_sw_tbl back;
	from_json(j, back);
	EXPECT_EQ(back.cond, SW_COND_DARK);

	press(4);
	EXPECT_TRUE(dst->pin_is_on(1));

	// day: not switched on, but still off
	EXPECT_EQ(fs_ops.write("/switches/dark", "0\n", 2, 0, nullptr), 2);
	EXPECT_EQ(dark(), "0\n");
	EXPECT_FALSE(swHdl.is_dark());
	EXPECT_NE(list().find("switches, day\n"), std::string::npos) << buf;
	press(5);
	EXPECT_FALSE(dst->pin_is_on(2));
	EXPECT_FALSE(swHdl.timer_running(dst_pio(2, 2, 2)));
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));
	// added again without "dark": always
	fs_ops.write("/switches/add", "1.2.4 2.2.1", 12, 0, nullptr);
	press(4);
	EXPECT_TRUE(dst->pin_is_on(1));

	// from the devices interface, as the jsengine sets it
	ow.dark_set(true);
	EXPECT_TRUE(ow.is_dark());
	press(5);
	EXPECT_TRUE(dst->pin_is_on(2));
	EXPECT_TRUE(swHdl.timer_running(dst_pio(2, 2, 2)));

	EXPECT_EQ(fs_ops.write("/switches/dark", "2", 1, 0, nullptr), -EINVAL);
	EXPECT_EQ(fs_ops.write("/switches/dark", "night", 5, 0, nullptr), -EINVAL);
	EXPECT_TRUE(swHdl.is_dark());

	const char* del = "1.2.4 2.2.1\n1.2.5 2.2.2\n1.2.6 0.0.0";
	fs_ops.write("/switches/del", del, strlen(del) + 1, 0, nullptr);
	swHdl.timer_poll(HrClock::now() + std::chrono::seconds(60));
	dst->pin_switch(1, OFF);
}

// a dark switch goes by the dark flag of its target's room, while that
// room has one of its own
TEST_F(SwTest, RoomDarkSwitches)
{
	char buf[2048];
	uint8_t adr[] = { 0x29, 0x02, 0x01, 0xab, 0xbd, 0x66, 0x77, 0x9a };
	ds2408* src = (ds2408*)ow.find(0x290201ABBD66779A);
	ds2408* dst = (ds2408*)ow.find(0x290202ABBD6677D4);
	ASSERT_NE(src, nullptr);
	ASSERT_NE(dst, nullptr);
	for (int i = 0; i < 4; i++)
		dst->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	dst->pin_switch(1, OFF);
	dst->pin_switch(2, OFF);
	auto press = [&](int latch) {
		src->data[STAT] = 0;
		src->data[PIO_TIME] = 0xff;
		src->data[PIO_LATCH] = 1 << (latch - 1);
		swHdl.dev_alarm(1, adr);
	};
	auto list = [&]() {
		int n = fs_ops.read("/switches/list", buf, sizeof(buf), 0, nullptr);
		return std::string(buf, n > 0 ? n : 0);
	};

	fs_ops.write("/29.0202ABBD6677D4/pin.1/room", "Kitchen", 7, 0, nullptr);
	const char* add = "1.2.4 2.2.1 dark\n1.2.5 2.2.2 dark";
	fs_ops.write("/switches/add", add, strlen(add) + 1, 0, nullptr);

	// day outside, but the kitchen is dark: only 2.2.1 is in the kitchen
	ow.dark_set(false);
	ow.dark_set(true, "kitchen");
	EXPECT_NE(list().find("1.2.4 -> 2.2.1 dark: 29.0201ABBD66779A, PIO.3 (latch 4) -> 29.0202ABBD6677D4, PIO.1, only when dark (kitchen dark)\n"),
		std::string::npos) << buf;
	EXPECT_NE(list().find("PIO.2, only when dark\n"), std::string::npos) << buf;
	press(4);
	EXPECT_TRUE(dst->pin_is_on(1));
	press(5);
	EXPECT_FALSE(dst->pin_is_on(2));
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));

	// the other way round, through the file system
	ow.dark_set(true);
	EXPECT_EQ(fs_ops.write("/rooms/kitchen/dark", "0", 1, 0, nullptr), 1);
	EXPECT_FALSE(ow.is_dark("kitchen"));
	press(4);
	EXPECT_FALSE(dst->pin_is_on(1));
	press(5);
	EXPECT_TRUE(dst->pin_is_on(2));

	// following the astro flag again
	ow.dark_clear("kitchen");
	EXPECT_TRUE(ow.is_dark("kitchen"));
	press(4);
	EXPECT_TRUE(dst->pin_is_on(1));
	EXPECT_NE(list().find("PIO.1, only when dark\n"), std::string::npos) << buf;

	const char* del = "1.2.4 2.2.1\n1.2.5 2.2.2";
	fs_ops.write("/switches/del", del, strlen(del) + 1, 0, nullptr);
	fs_ops.write("/29.0202ABBD6677D4/pin.1/room", "", 0, 0, nullptr);
	dst->pin_switch(1, OFF);
	dst->pin_switch(2, OFF);
}
