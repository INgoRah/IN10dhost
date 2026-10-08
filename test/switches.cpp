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

extern DS2482 ds;
extern OwDevices ow;
extern SwitchHandler swHdl;
// defined in src/switch_handler.cpp
extern void to_json(json& j, const _sw_tbl& b);
extern void from_json(const json& j, _sw_tbl& b);

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
