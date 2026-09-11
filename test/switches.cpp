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
		ow.begin(&ds);
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

	// alarmHandler() is a no-op under USE_I2C=OFF, never called from
	// anywhere else in this build
	EXPECT_EQ(swHdl.alarmHandler(0), false);
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