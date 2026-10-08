#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include <fstream>
#include <fuse3/fuse.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"
#include "ds1820.h"
#include "ds2450.h"
#include "ard_i2c.h"
#include "fs_table.h"
#include "plugins.h"
#include "switch_handler.h"

extern OwDevices ow;
extern DS2482 ds;
extern Plugins plugins;
extern SwitchHandler swHdl;
// defined in test/plugins.cpp
extern std::filesystem::path exec_path();
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

class FsTest : public ::testing::Test {
protected:
	void SetUp() override {
		fs_init(&fs_ops);
		ow.init();
		ow.begin();
	}
};

// Test root directory attributes
TEST_F(FsTest, GetAttrRootReturnsDirectory) {
	char buf[1024];
	struct stat st;
	int res = fs_ops.getattr("/", &st, nullptr);

	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.readdir("/", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
}

// Test path parsing for "bus.X" logic
TEST_F(FsTest, GetAttrBusDirReturnsDirectory) {
	int res;
	struct stat st;
	// Mock 1 bus available
	//EXPECT_CALL(ow, bus_count()).WillRepeatedly(testing::Return(1));
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	res = fs_ops.getattr("/bus.1", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.getattr("/bus.1/29.0701F8FE6677F4", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.open("/bus.1/29.0701F8FE6677F4/BYTE",  nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/bus.1/29.0701F8FE6677F4/BYTE", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 3);
}

// Test device file attribute retrieval
TEST_F(FsTest, GetDS2408Devices) {
	char buf[1024];
	int res;
	struct stat st;
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// TODO: write cfg for pins with 0x21, 0x23, 0x10 and read dir
	res = fs_ops.readdir("/29.0701F8FE6677F4", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);

	res = fs_ops.getattr("/29.0701F8FE6677F4/BYTE", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 3);
	//res = fs_ops.readdir("/29.0701F8FE6677F4/", &st, nullptr);
	//EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/29.0701F8FE6677F4/", &st, nullptr);
	EXPECT_TRUE(S_ISDIR(st.st_mode));

}

TEST_F(FsTest, GetDS1820Devices) {
	char buf[1024];
	int res;
	struct stat st;

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::VERBOSE);

	ow.update_device(1, "28.0501FAFE6677A0");
	ow.update_data();
	logger.verbose("data updated");
	res = fs_ops.readdir("/28.0501FAFE6677A0", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	res = fs_ops.open("/28.0501FAFE6677A0/temperature", nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/28.0501FAFE6677A0/temperature", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 5);
	res = fs_ops.read("/28.0501FAFE6677A0/temperature", buf, 32, 0, nullptr);
	EXPECT_GE(res, 4);
	res = fs_ops.getattr("/28.0501FAFE6677A0/humidity", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 4);
	res = fs_ops.read("/28.0501FAFE6677A0/humidity", buf, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	res = fs_ops.getattr("/bus.1/28.0501FAFE6677A0/temperature", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	logger.error("read");
	// uncached is accessing the device
	ow.begin();
	res = fs_ops.read("/uncached/28.0501FAFE6677A0/temperature", buf, 32, 0, nullptr);
	// set log level back if changed by test
	logger.set_level(lvl);
#if 0
	EXPECT_EQ(res, 2);
	res = fs_ops.getattr("/28.0501FAFE6677A0/humidity", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 4);
#endif
}

TEST_F(FsTest, DS1820SetAlarms) {
	// flag 2 ("set alarms and reset") is never reached through fs_read
	// (which only ever passes 0 or 1); exercised directly here
	ow.update_device(1, "28.0501FAFE6677A0");
	ow.update_data();
	ds1820* dev = (ds1820*)ow.find(1, 5, 0x28);
	ASSERT_NE(dev, nullptr);
	// temp_read() locks ds->mtx; ds is only assigned by begin(), which
	// update_device() deliberately skips (see its comment) - without
	// this, dev->ds is still nullptr and temp_read() segfaults
	ow.begin();
	EXPECT_EQ(dev->temp_read(2), 0);
}

TEST_F(FsTest, GetDS2450Devices) {
	char buf[1024];
	int res;
	struct stat st;

	ow.update_device(0, "20.0200F8FE66771E");
	ow.update_data();
	// the loop below reads "/uncached/...", which needs dev->ds; only
	// begin() assigns it, and update_device() deliberately doesn't call
	// it (see its comment)
	ow.begin();
	res = fs_ops.readdir("/20.0200F8FE66771E", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	for (char c = 'A'; c <= 'D'; c++) {
		string path = "/20.0200F8FE66771E/volt.";
		path += c;
		res = fs_ops.open(path.c_str(), nullptr);
		EXPECT_EQ(res, 0);
		res = fs_ops.getattr(path.c_str(), &st, nullptr);
		EXPECT_EQ(res, 0);
		EXPECT_TRUE(S_ISREG(st.st_mode));
		EXPECT_GE(st.st_size, 4);
		res = fs_ops.read(path.c_str(), buf, 32, 0, nullptr);
		EXPECT_GE(res, 4);
		path = "/uncached/20.0200F8FE66771E/volt.";
		path += c;
		res = fs_ops.read(path.c_str(), buf, 32, 0, nullptr);
		EXPECT_GE(res, 4);
	}
}

// Test device file attribute retrieval
TEST_F(FsTest, GetAttrDeviceFile) {
	int res;
	struct stat st;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	res = fs_ops.getattr("/29.0701F8FE6677F4/name", &st, nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/bus.1/29.0701F8FE6677F4/BYTE", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 3);
	res = fs_ops.getattr("/bus.1/29.0701F8FE6677F4/", &st, nullptr);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// Test "uncached" triggers search
TEST_F(FsTest, ReaddirUncachedTriggersSearch) {

	auto mock_filler = [](void* buf, const char* name, const struct stat* st,
						  off_t off, enum fuse_fill_dir_flags flags) {
		(void)buf; (void)name; (void)st; (void)off; (void)flags;
		return 0;
	};

	fs_ops.readdir("/uncached", nullptr, mock_filler, 0, nullptr, FUSE_READDIR_PLUS);
}

// Test "uncached" triggers search
TEST_F(FsTest, ReadAlarms)
{
	struct stat st;
	int res;

	res = fs_ops.getattr("/alarm", &st, nullptr);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.readdir("/alarm", nullptr, filler, 0, nullptr, FUSE_READDIR_PLUS);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, DevDirs) {
	struct stat st;
	int res;
	char buf[128];

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	res = fs_ops.getattr("/29.0701F8FE6677F4/", &st, nullptr);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.read("/29.0701F8FE6677F4/", buf, 32, 0, nullptr);
	EXPECT_GE(res, 0);
	// name
	res = fs_ops.getattr("/29.0701F8FE6677F4/name", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	buf[0] = 't';
	buf[1] = 'e';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/name", buf, 3, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/name", buf, 32, 0, nullptr);
	EXPECT_STREQ(buf, "te");
	// cfg
	res = fs_ops.getattr("/29.0701F8FE6677F4/cfg", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.read("/29.0701F8FE6677F4/cfg", buf, 32, 0, nullptr);
	EXPECT_GE(res, 0);
	// uncached is accessing the device
	ow.begin();
	res = fs_ops.read("/uncached/29.0701F8FE6677F4/cfg", buf, 32, 0, nullptr);
	EXPECT_GE(res, 0);

	res = fs_ops.readdir("/29.0701F8FE6677F4/", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_GE(res, 0);
	// todo check for PIO.0
	res = fs_ops.getattr("/29.0701F8FE6677F4/PIO.0", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GE(res, 0);
	res = fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);
	//EXPECT_STREQ(buf, "1");
	res = fs_ops.getattr("/29.0701F8FE6677F4/sensed.0", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.read("/29.0701F8FE6677F4/sensed.0", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);

	res = fs_ops.read("/29.0701F8FE6677F4/pin.0", buf, 32, 0, nullptr);
	EXPECT_GE(res, 0);
	res = fs_ops.read("/29.0701F8FE6677F4/latched.0", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);
	res = fs_ops.write("/29.0701F8FE6677F4/latched.0", buf, 3, 0, nullptr);
	EXPECT_GE(res, 0);
}

TEST_F(FsTest, DevPinDirs) {
	struct stat st;
	int res;
	char buf[128];

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	// pin dirs
	res = fs_ops.getattr("/29.0701F8FE6677F4/pin.0/name", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GE(res, 0);
	buf[0] = 't';
	buf[1] = 'e';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/name", buf, 3, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/pin.0/name", buf, 32, 0, nullptr);
	EXPECT_STREQ(buf, "te");

	res = fs_ops.getattr("/29.0701F8FE6677F4/pin.0", &st, nullptr);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.readdir("/29.0701F8FE6677F4/pin.0/", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_GE(res, 0);

	res = fs_ops.getattr("/29.0701F8FE6677F4/pin.0/func", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GE(res, 0);
	buf[0] = '3';
	buf[1] = '3';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", buf, 3, 0, nullptr);
	res = fs_ops.getattr("/29.0701F8FE6677F4/pin.0/func", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GE(res, 0);
	// check for input (no PIO)
	buf[0] = '1';
	buf[1] = '6';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", buf, 3, 0, nullptr);
	res = fs_ops.getattr("/29.0701F8FE6677F4/pin.0/func", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GE(res, 0);
	res = fs_ops.readdir("/29.0701F8FE6677F4/", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_GE(res, 0);
	// todo check for PIO.0
	res = fs_ops.getattr("/29.0701F8FE6677F4/PIO.0", &st, nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	//EXPECT_EQ(res, -1);

	// func reads back known pin functions by name, anything else as
	// hex. Written in decimal, see w_pin_func().
	const struct {
		const char* in;
		const char* out;
	} funcs[] = {
		{ "33", "OUT" },	// 0x21
		{ "35", "PWM" },	// 0x23
		{ "16", "BTN" },	// 0x10
		{ "17", "SW" },		// 0x11
		{ "5", "PASS" },	// 0x05
		{ "6", "INV" },		// 0x06
		{ "7", "INV_PU" },	// 0x07
		{ "2", "ACT_HIGH" },	// 0x02
		{ "34", "22" },		// 0x22, no name
		{ "171", "AB" },	// 0xAB, no name
		{ "0", "0" },
	};
	for (const auto& f : funcs) {
		for (int pin : { 0, 7 }) {
			std::string path = "/29.0701F8FE6677F4/pin." + std::to_string(pin) + "/func";
			res = fs_ops.write(path.c_str(), f.in, strlen(f.in) + 1, 0, nullptr);
			EXPECT_EQ(res, (int)strlen(f.in) + 1);
			buf[0] = '\0';
			res = fs_ops.read(path.c_str(), buf, 32, 0, nullptr);
			EXPECT_EQ(res, (int)strlen(f.out)) << path << " = " << f.in;
			EXPECT_STREQ(buf, f.out) << path << " = " << f.in;
		}
	}
	// pins are independent of each other
	fs_ops.write("/29.0701F8FE6677F4/pin.0/func", "33", 3, 0, nullptr);
	fs_ops.write("/29.0701F8FE6677F4/pin.1/func", "16", 3, 0, nullptr);
	fs_ops.read("/29.0701F8FE6677F4/pin.0/func", buf, 32, 0, nullptr);
	EXPECT_STREQ(buf, "OUT");
	fs_ops.read("/29.0701F8FE6677F4/pin.1/func", buf, 32, 0, nullptr);
	EXPECT_STREQ(buf, "BTN");
}

TEST_F(FsTest, WriteReadDev) {
	int res;
	char buf[128];

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	res = fs_ops.read("/29.0701F8FE6677F4/name", buf, 2, 0, nullptr);
	buf[0] = 'I';
	buf[1] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/name", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	res = fs_ops.read("/29.0701F8FE6677F4/id", buf, 2, 0, nullptr);
	buf[0] = 'I';
	buf[1] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/id", buf, 2, 0, nullptr);
	EXPECT_EQ(res, -1);
	buf[0] = '7';
	res = fs_ops.write("/29.0701F8FE6677F4/id", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	res = fs_ops.read("/29.0701F8FE6677F4/cfg", buf, 128, 0, nullptr);
	EXPECT_GE(res, 70);
	buf[0] = '3';
	buf[1] = '3';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", buf, 3, 0, nullptr);
	res = fs_ops.write("/29.0701F8FE6677F4/pin.1/func", buf, 3, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/pin.0/func", buf, 32, 0, nullptr);
	EXPECT_EQ(res, 3);
	// interpret as hex
	EXPECT_STREQ(buf, "OUT");
}

TEST_F(FsTest, WriteReadDevPio) {
	//LogLevel lvl = logger.get_level();
	char buf[128];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// the uncached BYTE read below needs dev->ds; only begin() assigns
	// it, and update_device() deliberately doesn't call it (see its
	// comment)
	ow.begin();

	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	// read all pios
	for (int i = 0; i < 8; i++) {
		std::string fpath = "/29.0701F8FE6677F4/pin." + std::to_string(i) + "/func";
		// set to output
		buf[0] = '3';
		buf[1] = '3';
		buf[2] = '\0';
		res = fs_ops.write(fpath.c_str(), buf, 3, 0, nullptr);

		dev->data[PIO_OUT] |= (0x1 << i);
		dev->data[PIO_LATCH] = 0x01;

		std::string path = "/29.0701F8FE6677F4/PIO." + std::to_string(i);
		res = fs_ops.read(path.c_str(), buf, 2, 0, nullptr);
		EXPECT_EQ(res, 1);
		// PIO is inverted: latch bit 1 reads as 0
		EXPECT_STREQ(buf, "0");
		dev->data[PIO_OUT] &= ~(0x1 << i);
		res = fs_ops.read(path.c_str(), buf, 2, 0, nullptr);
		EXPECT_EQ(res, 1);
		EXPECT_STREQ(buf, "1");
	}

	buf[0] = '3';
	buf[1] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	buf[0] = '\0';
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);
	EXPECT_EQ(buf[0], '3');
	res = fs_ops.read("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);
	EXPECT_EQ(buf[0], '0');

	buf[0] = '1';
	buf[1] = '\0';

	// byte = 0x1 -> PIO.0 = 0 (on), PIO.1 = 1 (off), PIO is inverted
	fs_ops.write("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '0');
	fs_ops.read("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '1');
	// PIO.1 = 0 sets its latch bit (PIO.0 unchanged) -> byte = 0x3
	buf[0] = '0';
	fs_ops.write("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '3');
	fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '0');
	fs_ops.read("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '0');

	buf[0] = '1';
	fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '2');
	buf[0] = '1';
	fs_ops.write("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	dev->data[PIO_OUT] = 222;
	res = fs_ops.read("/uncached/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "222");
}

// Standalone check of fs_table.h's alpha wildcard mapping (A -> 0,
// B -> 1, ...), decoupled from any real device: ds2450's own volt_a..
// volt_d are private with no test setter, and the simulated 1-Wire bus
// does not differentiate its canned response by channel, so neither
// can prove the letter-to-index mapping is actually correct end to
// end. This exercises fs_table::read()/write() directly against a
// throwaway table instead.
namespace {
struct AlphaProbe {
	int seen_idx[4] = { -1, -1, -1, -1 };
	int r_ch(char* buf, size_t, bool, int idx)
	{
		std::sprintf(buf, "%d", idx);
		return std::strlen(buf);
	}
	int w_ch(const char* buf, size_t size, int idx)
	{
		if (idx >= 0 && idx < 4)
			seen_idx[idx] = std::atoi(buf);
		return size;
	}
	static const FsEntry<AlphaProbe> table[];
	static const size_t n_table;
};
const FsEntry<AlphaProbe> AlphaProbe::table[] = {
	{ "ch.*", 2, 4, /* alpha */ true, nullptr, nullptr, &AlphaProbe::r_ch, &AlphaProbe::w_ch },
};
const size_t AlphaProbe::n_table = sizeof(AlphaProbe::table) / sizeof(AlphaProbe::table[0]);
}

TEST(FsTableAlpha, LettersMapToZeroBasedIndex) {
	AlphaProbe p;
	char buf[8];

	// A -> 0, B -> 1, C -> 2, D -> 3, and each stays independent of
	// the others - a wrong mapping (e.g. an off-by-one, or all four
	// letters aliasing to the same idx) would show up here
	const char* letters = "ABCD";
	for (int i = 0; i < 4; i++) {
		std::string path = std::string("ch.") + letters[i];
		int r = fs_table::read(p, AlphaProbe::table, AlphaProbe::n_table, path, buf, sizeof(buf), false);
		EXPECT_GE(r, 0);
		EXPECT_EQ(std::string(buf, r), std::to_string(i));
	}

	// and the same mapping applies to write()
	for (int i = 0; i < 4; i++) {
		std::string path = std::string("ch.") + letters[i];
		std::string val = std::to_string(100 + i);
		int r = fs_table::write(p, AlphaProbe::table, AlphaProbe::n_table, path, val.c_str(), val.size());
		EXPECT_GE(r, 0);
	}
	EXPECT_EQ(p.seen_idx[0], 100);
	EXPECT_EQ(p.seen_idx[1], 101);
	EXPECT_EQ(p.seen_idx[2], 102);
	EXPECT_EQ(p.seen_idx[3], 103);
}

TEST_F(FsTest, Ds2408PioVisibility) {
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);

	// pin 0 not yet configured as an output/input: PIO.0 and sensed.0
	// are reachable directly (fs_attr/fs_read never gate on cfg) but
	// must not appear in the directory listing
	dev->cfg[CFG_PIN_ID] = 0;
	seen.clear();
	fs_ops.readdir("/29.0701F8FE6677F4", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(std::find(seen.begin(), seen.end(), "PIO.0"), seen.end());
	EXPECT_EQ(std::find(seen.begin(), seen.end(), "sensed.0"), seen.end());
	struct stat st;
	EXPECT_EQ(fs_ops.getattr("/29.0701F8FE6677F4/PIO.0", &st, nullptr), 0);

	// configure pin 0 as an output: PIO.0 must now be listed, sensed.0
	// (an input-only concept) still must not
	dev->cfg[CFG_PIN_ID] = CFG_OUT_LOW;
	seen.clear();
	fs_ops.readdir("/29.0701F8FE6677F4", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "PIO.0"), seen.end());
	EXPECT_EQ(std::find(seen.begin(), seen.end(), "sensed.0"), seen.end());

	// configure pin 0 as an input/button: sensed.0 now listed, PIO.0 not
	dev->cfg[CFG_PIN_ID] = CFG_BTN;
	seen.clear();
	fs_ops.readdir("/29.0701F8FE6677F4", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "sensed.0"), seen.end());
	EXPECT_EQ(std::find(seen.begin(), seen.end(), "PIO.0"), seen.end());

	// latched.* and pin.* are unconditional regardless of cfg
	EXPECT_NE(std::find(seen.begin(), seen.end(), "latched.0"), seen.end());
	EXPECT_NE(std::find(seen.begin(), seen.end(), "pin.0"), seen.end());
}

TEST_F(FsTest, Ds2408PinInstanceDirLists) {
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

	// browsing into a pin instance returns exactly its two children,
	// not the device's own top-level files mixed in
	fs_ops.readdir("/29.0701F8FE6677F4/pin.0", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	ASSERT_EQ(seen.size(), 2u);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "name"), seen.end());
	EXPECT_NE(std::find(seen.begin(), seen.end(), "func"), seen.end());
}

TEST_F(FsTest, Ds2408WriteDoesNotCrossContaminate) {
	char buf[32];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);

	// writing to a pin's "name" file must not fall through to the
	// device's own name field (both paths contain the substring "name")
	dev->name = "original";
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/name", (char*)"hijack", 6, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_EQ(dev->name, "original");

	// writing to sensed.0 (a read-only sensor value) must not reset the
	// activity latches the way writing to latched.0 deliberately does
	dev->data[PIO_LATCH] = 0xAB;
	res = fs_ops.write("/29.0701F8FE6677F4/sensed.0", (char*)"1", 1, 0, nullptr);
	EXPECT_GE(res, 0);
	EXPECT_EQ(dev->data[PIO_LATCH], 0xAB);

	// read confirms buf untouched by the write-to-name call above
	res = fs_ops.read("/29.0701F8FE6677F4/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "original");
}

TEST_F(FsTest, Ds2408Gaps) {
	int res;
	struct stat st;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// cfg_write() below needs dev->ds; only begin() assigns it, and
	// update_device() deliberately doesn't call it (see its comment)
	ow.begin();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);

	// "latched.N" attribute is queried by getattr but not exercised by
	// any existing read/write test
	res = fs_ops.getattr("/29.0701F8FE6677F4/latched.0", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));

	// non-numeric BYTE write
	res = fs_ops.write("/29.0701F8FE6677F4/BYTE", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// BYTE write that parses but doesn't fit in an int: w_byte()'s
	// std::out_of_range branch, unlike its std::invalid_argument one
	// above, was never exercised
	res = fs_ops.write("/29.0701F8FE6677F4/BYTE", (char*)"99999999999999999999", 21, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// same two exceptions on w_pio(), neither previously exercised
	res = fs_ops.write("/29.0701F8FE6677F4/PIO.0", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/29.0701F8FE6677F4/PIO.0", (char*)"99999999999999999999", 21, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// same two exceptions on w_pin_func(), neither previously exercised
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", (char*)"99999999999999999999", 21, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// never exercised: writes the device's cfg block back over the bus
	EXPECT_EQ(dev->cfg_write(dev->cfg), DS2408_CFG_SIZE);

	// OwDev::fs_attr()'s empty-path guard: no FUSE caller ever passes
	// an empty path (the ROM prefix is always still attached), so it's
	// only reachable by calling the base implementation directly
	std::string empty;
	EXPECT_EQ(dev->fs_attr(empty), 0);

	// a generic (non-"name") attribute, e.g. "poll", takes the
	// suglen-return branch that "name" itself doesn't
	res = fs_ops.getattr("/29.0701F8FE6677F4/poll", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));

	// non-numeric "poll" write
	res = fs_ops.write("/29.0701F8FE6677F4/poll", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -1);
}

TEST_F(FsTest, WriteReadDevLevel) {
	LogLevel lvl = logger.get_level();
	char buf[128];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// writing BYTE/PIO.* below goes over the (simulated) bus via
	// pio_set(), which needs dev->ds; only begin() assigns it, and
	// update_device() deliberately doesn't call it (see its comment)
	ow.begin();
	buf[0] = '3';
	buf[1] = '5';
	buf[2] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/pin.0/func", buf, 3, 0, nullptr);
	EXPECT_EQ(res, 3);

	buf[0] = '0';
	buf[1] = '\0';
	fs_ops.write("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	//logger.set_level(LogLevel::VERBOSE);
	fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	buf[0] = '5';
	buf[1] = '0';
	buf[2] = '\0';
	fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 3, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "50");
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	buf[0] = '0';
	buf[1] = '\0';
	fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	logger.set_level(lvl);
}

// Test root directory attributes
TEST_F(FsTest, SettingsDirectoryAttr) {
	struct stat st;
	int res = fs_ops.getattr("/settings", &st, nullptr);

	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.open("/settings/log",  nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/settings/log", &st, nullptr);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	LogLevel prev_lvl = logger.get_level();
	char buf[3];
	buf[0] = '7';
	buf[1] = '\0';
	res = fs_ops.write("/settings/log", buf, 2, 0, nullptr);
	int lvl = (int)logger.get_level();
	fs_ops.read("/settings/log", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '0' + lvl);
	buf[0] = '3';
	res = fs_ops.write("/settings/log", buf, 2, 0, nullptr);
	lvl = (int)logger.get_level();
	EXPECT_EQ(lvl, 3);
	buf[0] = '9';
	res = fs_ops.write("/settings/log", buf, 2, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	logger.set_level(prev_lvl);

	res = fs_ops.getattr("/settings/poll", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.open("/settings/poll",  nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.read("/settings/poll", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 1);
	buf[0] = '1';
	buf[1] = '0';
	buf[2] = '\0';
	res = fs_ops.write("/settings/poll", buf, 3, 0, nullptr);
	EXPECT_EQ(res, 3);
}

// Test root directory attributes
TEST_F(FsTest, SettingsDirectory) {
	char buf[1024];

	struct stat st;
	int res = fs_ops.getattr("/settings", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.readdir("/settings", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, PseudoArduinoDev) {
	//LogLevel lvl = logger.get_level();
	struct stat st;
	char buf[128];
	int res;

	ow.update_device(1, "AD.0900F8FF6677E2");
	ow.update_data();
	ow.begin();
	res = fs_ops.getattr("/AD.0900F8FF6677E2", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	res = fs_ops.readdir("/AD.0900F8FF6677E2", nullptr, filler, 0, nullptr, FUSE_READDIR_PLUS);
	EXPECT_EQ(res, 0);

	res = fs_ops.getattr("/AD.0900F8FF6677E2/power", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.read("/AD.0900F8FF6677E2/power", buf, 32, 0, nullptr);
	// set to output
	buf[0] = '1';
	buf[1] = '6';
	buf[2] = '\0';
	res = fs_ops.getattr("/AD.0900F8FF6677E2/mode", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_FALSE(S_ISDIR(st.st_mode));
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.write("/AD.0900F8FF6677E2/mode", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	res = fs_ops.read("/AD.0900F8FF6677E2/mode", buf, 32, 0, nullptr);
	EXPECT_EQ(res, 2);
	EXPECT_STREQ(buf, "16");
	res = fs_ops.write("/AD.0900F8FF6677E2/test", buf, 1, 0, nullptr);
	EXPECT_EQ(res, 1);

	buf[0] = '3';
	buf[1] = '\0';
	//res = fs_ops.write("/AD.0900F8FF6677E2/power", buf, 2, 0, nullptr);
	//EXPECT_EQ(res, 2);

	// interrupt-handling timing stats, no samples recorded yet
	res = fs_ops.read("/AD.0900F8FF6677E2/int_min", buf, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "0");
	res = fs_ops.read("/AD.0900F8FF6677E2/int_max", buf, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "0");
	res = fs_ops.read("/AD.0900F8FF6677E2/int_avg", buf, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "0");

	// paths not handled by Ard_i2c itself fall back to the base OwDev
	// read/write implementation
	res = fs_ops.write("/AD.0900F8FF6677E2/name", (char*)"arduino", 8, 0, nullptr);
	EXPECT_GT(res, 0);
	res = fs_ops.read("/AD.0900F8FF6677E2/name", buf, 32, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_STREQ(buf, "arduino");

	// device lifecycle / interrupt handling, unreachable through the
	// FUSE read/write API
	Ard_i2c* arduino = (Ard_i2c*)ow.find(1, 9, 0xAD);
	ASSERT_NE(arduino, nullptr);
	arduino->begin();
	// no-ops without USE_I2C, just exercised for coverage
	arduino->interrupt();
	arduino->end();
}

TEST_F(FsTest, MalformedBusPaths) {
	struct stat st;
	int res;

	// "bus." with no digits following it at all
	res = fs_ops.getattr("/bus.x", &st, nullptr);
	EXPECT_EQ(res, -ENOENT);
	// well-formed but out of MAX_BUS range
	res = fs_ops.getattr("/bus.99", &st, nullptr);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, LogDirectory) {
	struct stat st;
	int res;

	res = fs_ops.getattr("/log", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));

	res = fs_ops.getattr("/log/1wire.vcd", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GT(st.st_size, 0);

	res = fs_ops.readdir("/log", nullptr, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);

	res = fs_ops.open("/log/1wire.vcd", nullptr);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, ReaddirBusLevel) {
	int res;

	ow.update_device(0, "29.0300FDFF6677F9");
	ow.update_data();
	// lists every device registered on bus 0, exercising the bus-level
	// (as opposed to device-level) branch of fs_readdir
	res = fs_ops.readdir("/bus.0", nullptr, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, ReaddirBusLevelFiltersByBus) {
	// readdir("/bus.N") must list only devices whose own bus is N -
	// ReaddirBusLevel above only checks the return code, not which
	// devices actually show up, so it would miss a device from another
	// bus leaking into the listing
	ow.update_device(0, "29.0600FDFF6677F0");
	ow.update_device(1, "29.0700FDFF6677F1");
	ow.update_data();
	// update() overwrites the ROM's last byte with a real CRC8 (see
	// OwDev::update()), so the string above is not what ends up
	// registered - look the devices up instead of assuming it
	OwDev* dev0 = ow.find(0, 0x06, 0x29);
	OwDev* dev1 = ow.find(1, 0x07, 0x29);
	ASSERT_NE(dev0, nullptr);
	ASSERT_NE(dev1, nullptr);

	auto record_filler = [](void* buf, const char* name, const struct stat*,
							 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};

	std::vector<std::string> seen0;
	int res = fs_ops.readdir("/bus.0", &seen0, record_filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	EXPECT_NE(std::find(seen0.begin(), seen0.end(), dev0->rom), seen0.end());
	EXPECT_EQ(std::find(seen0.begin(), seen0.end(), dev1->rom), seen0.end());

	std::vector<std::string> seen1;
	res = fs_ops.readdir("/bus.1", &seen1, record_filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	EXPECT_NE(std::find(seen1.begin(), seen1.end(), dev1->rom), seen1.end());
	EXPECT_EQ(std::find(seen1.begin(), seen1.end(), dev0->rom), seen1.end());
}

TEST_F(FsTest, ReaddirDeviceLevelErrors) {
	int res;

	// subpath under a device directory that isn't a well-formed ROM
	res = fs_ops.readdir("/not-a-rom", nullptr, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, -ENOENT);
	// well-formed ROM shape, but never registered
	res = fs_ops.readdir("/00.000000000000AA/x", nullptr, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, OpenFallback) {
	// not a settings/log path, not a device ROM: falls back to
	// SwitchHandler::fs_open()
	int res = fs_ops.open("/switches/list", nullptr);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, SettingsWriteErrors) {
	int res;

	res = fs_ops.write("/settings/log", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/settings/poll", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// /settings/plugins is a directory, so it is not a write target
	// itself - the write falls through to the generic device/switch
	// handling, which does not recognize this path either
	res = fs_ops.write("/settings/plugins", (char*)"", 0, 0, nullptr);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, PluginDirLists) {
	struct stat st;
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};

	// the plugins entry is a directory now, not a 1024 byte file
	int res = fs_ops.getattr("/settings/plugins", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));

	plugins.add("example");
	res = fs_ops.readdir("/settings/plugins", &seen, record, 0, nullptr,
		(enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "example"), seen.end());
}

TEST_F(FsTest, PluginFileAttrAndRead) {
	struct stat st;
	char buf[1024];

	plugins.add("example");

	// a loaded plugin shows up as a regular file ...
	int res = fs_ops.getattr("/settings/plugins/example", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_GT(st.st_size, 0);
	EXPECT_EQ(fs_ops.open("/settings/plugins/example", nullptr), 0);

	// ... whose contents are its config
	res = fs_ops.read("/settings/plugins/example", buf, sizeof(buf), 0, nullptr);
	ASSERT_GT(res, 0);
	EXPECT_NE(std::string(buf, res).find("enabled"), std::string::npos);

	// one that is not loaded does not exist
	res = fs_ops.getattr("/settings/plugins/no_such_plugin", &st, nullptr);
	EXPECT_EQ(res, -ENOENT);
	// and neither does anything nested below a plugin
	res = fs_ops.getattr("/settings/plugins/example/deeper", &st, nullptr);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, PluginCreateLoadsAndRmUnloads) {
	struct stat st;

	// start from a known state: not loaded
	plugins.remove("example");
	EXPECT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), -ENOENT);

	// touch loads it (create + utimens are what touch actually calls)
	EXPECT_EQ(fs_ops.create("/settings/plugins/example", 0666, nullptr), 0);
	EXPECT_EQ(fs_ops.utimens("/settings/plugins/example", nullptr, nullptr), 0);
	EXPECT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), 0);
	// touching it again is harmless
	EXPECT_EQ(fs_ops.create("/settings/plugins/example", 0666, nullptr), 0);

	// writing rm unloads it again, newline from a shell redirect included
	int res = fs_ops.write("/settings/plugins/example", (char*)"rm\n", 3, 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), -ENOENT);

	// removing it twice reports the second as missing
	EXPECT_EQ(fs_ops.write("/settings/plugins/example", (char*)"rm", 2, 0, nullptr), -ENOENT);

	plugins.add("example"); // restore for the other tests
}

TEST_F(FsTest, PluginTouchReloadsExisting) {
	struct stat st;

	plugins.add("example");
	ASSERT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), 0);

	// give the reload something to find: a trailing byte changes the
	// library's content hash without stopping the ELF from loading
	std::filesystem::path lib = exec_path() / "libexample.so";
	std::filesystem::path backup = exec_path() / "libexample.so.orig";
	std::filesystem::copy_file(lib, backup,
		std::filesystem::copy_options::overwrite_existing);
	{
		std::ofstream out(lib, std::ios::binary | std::ios::app);
		out.put('\0');
	}

	// touch on a file that already exists never reaches create(): the
	// kernel resolves it and calls open()+utimens(), so utimens is what
	// has to perform the reload
	EXPECT_EQ(fs_ops.utimens("/settings/plugins/example", nullptr, nullptr), 0);

	// it really reloaded: an explicit reload now finds nothing left to
	// do, whereas it would report one if utimens had skipped it
	EXPECT_EQ(plugins.reload(), 0);
	// and the plugin is still loaded and working
	EXPECT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), 0);
	EXPECT_EQ(plugins.action(ACT_READY), 1);

	std::filesystem::copy_file(backup, lib,
		std::filesystem::copy_options::overwrite_existing);
	std::filesystem::remove(backup);
	plugins.reload();

	// touching anything else is still just a no-op success, so plain
	// touch on the rest of the filesystem keeps working
	EXPECT_EQ(fs_ops.utimens("/settings/poll", nullptr, nullptr), 0);
}

TEST_F(FsTest, PluginUnlinkUnloads) {
	struct stat st;

	plugins.add("example");
	ASSERT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), 0);

	// rm on the plugin file unloads it
	EXPECT_EQ(fs_ops.unlink("/settings/plugins/example"), 0);
	EXPECT_EQ(fs_ops.getattr("/settings/plugins/example", &st, nullptr), -ENOENT);
	// a second rm has nothing left to remove
	EXPECT_EQ(fs_ops.unlink("/settings/plugins/example"), -ENOENT);

	// nothing outside the plugins directory may be deleted
	EXPECT_EQ(fs_ops.unlink("/settings/poll"), -EPERM);
	EXPECT_EQ(fs_ops.unlink("/29.0701F8FE6677F4/name"), -EPERM);

	plugins.add("example"); // restore for the other tests
}

TEST_F(FsTest, PluginWriteErrors) {
	plugins.add("example");

	LogLevel lvl = logger.get_level();
	logger.set_level(LogLevel::NONE);
	// only rm and reload are understood
	EXPECT_EQ(fs_ops.write("/settings/plugins/example", (char*)"wat", 3, 0, nullptr), -EINVAL);
	logger.set_level(lvl);

	// reload is accepted
	EXPECT_GT(fs_ops.write("/settings/plugins/example", (char*)"reload\n", 7, 0, nullptr), 0);

	// creating is only allowed under the plugins directory
	EXPECT_EQ(fs_ops.create("/settings/poll", 0666, nullptr), -EPERM);
	EXPECT_EQ(fs_ops.create("/29.0701F8FE6677F4/name", 0666, nullptr), -EPERM);
	// and only for a library that actually exists
	logger.set_level(LogLevel::NONE);
	EXPECT_EQ(fs_ops.create("/settings/plugins/no_such_plugin", 0666, nullptr), -ENOENT);
	logger.set_level(lvl);
}

TEST_F(FsTest, SettingsWriteOutOfRange) {
	// "nope" above exercises std::invalid_argument; a numeric string
	// that overflows int exercises the separate std::out_of_range catch
	int res;
	char buf[32] = "99999999999999999999";

	res = fs_ops.write("/settings/log", buf, strlen(buf), 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/settings/poll", buf, strlen(buf), 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
}

TEST_F(FsTest, OpenWrong) {
	int res = fs_ops.open("/blabla.txt", nullptr);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, ReadUnknownPathReturnsEnoent) {
	// not "switches", not a bus/rom path (no ".", so extractRom() never
	// matches): falls through every branch to the final -ENOENT
	char buf[32];
	int res = fs_ops.read("/totally/unknown/path", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, -ENOENT);
}

TEST_F(FsTest, DoubleSlashPathsAreNormalized) {
	// a stray extra leading slash - e.g. from a client that joins path
	// segments naively - leaves one "/" unconsumed by the generic
	// leading-slash strip every fs_* entry point does up front; both
	// extractRom() and extract_subpath() defensively absorb it rather
	// than failing the lookup
	ow.update_device(1, "29.1400FDFF6677F8");
	ow.update_data();
	OwDev* dev = ow.find(1, 0x14, 0x29);
	ASSERT_NE(dev, nullptr);
	char buf[32];

	std::string path = "//" + dev->rom + "/name";
	int res = fs_ops.read(path.c_str(), buf, sizeof(buf), 0, nullptr);
	EXPECT_GE(res, 0);

	path = "//uncached/" + dev->rom + "/name";
	res = fs_ops.read(path.c_str(), buf, sizeof(buf), 0, nullptr);
	EXPECT_GE(res, 0);
}

TEST_F(FsTest, AlarmReaddirSkipsNonAlarmingDevices) {
	// readdir("/alarm") lists only devices with alarm set; a device
	// present but not currently alarming must be filtered out
	ow.update_device(1, "29.1500FDFF6677F8");
	ow.update_data();
	OwDev* dev = ow.find(1, 0x15, 0x29);
	ASSERT_NE(dev, nullptr);
	dev->alarm = false;

	std::vector<std::string> seen;
	auto record_filler = [](void* buf, const char* name, const struct stat*,
							 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};
	int res = fs_ops.readdir("/alarm", &seen, record_filler, 0, nullptr, FUSE_READDIR_PLUS);
	EXPECT_EQ(res, 0);
	EXPECT_EQ(std::find(seen.begin(), seen.end(), dev->rom), seen.end());
}
TEST_F(FsTest, Ds2408ThresholdBrightness) {
	char buf[32];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// threshold_set()/brightness_set() go through level_set(), which
	// needs dev->ds - only begin() assigns it
	ow.begin();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);

	struct stat st;
	res = fs_ops.getattr("/29.0701F8FE6677F4/threshold", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.getattr("/29.0701F8FE6677F4/brightness", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));

	// defaults before anything was written or loaded
	ds2408 fresh("29.0701F8FE6677F4");
	EXPECT_EQ(fresh.to_json()["threshold"], 0);
	EXPECT_EQ(fresh.to_json()["brightness"], 0);

	std::strcpy(buf, "120");
	res = fs_ops.write("/29.0701F8FE6677F4/threshold", buf, 3, 0, nullptr);
	EXPECT_EQ(res, 3);

	// invalid input is rejected and leaves the value alone
	std::strcpy(buf, "abc");
	EXPECT_EQ(fs_ops.write("/29.0701F8FE6677F4/threshold", buf, 3, 0, nullptr), -EINVAL);
	EXPECT_EQ(fs_ops.write("/29.0701F8FE6677F4/brightness", buf, 3, 0, nullptr), -EINVAL);
	std::strcpy(buf, "99999999999");
	EXPECT_EQ(fs_ops.write("/29.0701F8FE6677F4/threshold", buf, 11, 0, nullptr), -EINVAL);
	res = fs_ops.read("/29.0701F8FE6677F4/threshold", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 3);
	EXPECT_STREQ(buf, "120");

	std::strcpy(buf, "80");
	res = fs_ops.write("/29.0701F8FE6677F4/brightness", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	res = fs_ops.read("/29.0701F8FE6677F4/brightness", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 2);
	EXPECT_STREQ(buf, "80");

	// both are independent of each other
	res = fs_ops.read("/29.0701F8FE6677F4/threshold", buf, sizeof(buf), 0, nullptr);
	EXPECT_STREQ(buf, "120");

	// only the low byte goes to the device
	std::strcpy(buf, "300");
	fs_ops.write("/29.0701F8FE6677F4/threshold", buf, 3, 0, nullptr);
	fs_ops.read("/29.0701F8FE6677F4/threshold", buf, sizeof(buf), 0, nullptr);
	EXPECT_STREQ(buf, "44");
	std::strcpy(buf, "120");
	fs_ops.write("/29.0701F8FE6677F4/threshold", buf, 3, 0, nullptr);

	// both are persisted and restored with the device config
	json j = dev->to_json();
	EXPECT_EQ(j["threshold"], 120);
	EXPECT_EQ(j["brightness"], 80);
	ds2408 restored;
	restored.from_json(j);
	json r = restored.to_json();
	EXPECT_EQ(r["threshold"], 120);
	EXPECT_EQ(r["brightness"], 80);
}

TEST_F(FsTest, ArduinoPower) {
	char buf[32];
	int res;
	struct stat st;

	ow.update_device(1, "AD.0900F8FF6677E2");
	ow.update_data();
	ow.begin();
	Ard_i2c* arduino = (Ard_i2c*)ow.find(1, 9, 0xAD);
	ASSERT_NE(arduino, nullptr);

	res = fs_ops.getattr("/AD.0900F8FF6677E2/power_total", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));

	// power and power_total are only updated from interrupt() on real
	// hardware, preset them the way it would
	arduino->power = 0;
	arduino->power_total = 0;
	res = fs_ops.read("/AD.0900F8FF6677E2/power", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 1);
	EXPECT_STREQ(buf, "0");
	res = fs_ops.read("/AD.0900F8FF6677E2/power_total", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 1);
	EXPECT_STREQ(buf, "0");

	arduino->power = 1800;
	arduino->power_total = 4242;
	res = fs_ops.read("/AD.0900F8FF6677E2/power", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 4);
	EXPECT_STREQ(buf, "1800");
	// "power" must not also match "power_total" and vice versa
	res = fs_ops.read("/AD.0900F8FF6677E2/power_total", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 4);
	EXPECT_STREQ(buf, "4242");

	// power_total is a counter that survives a restart, power is not
	json j = arduino->to_json();
	EXPECT_EQ(j["power_total"], 4242);
	EXPECT_FALSE(j.contains("power"));
	Ard_i2c restored;
	restored.from_json(j);
	EXPECT_EQ(restored.power_total, 4242);
	EXPECT_EQ(restored.power, 0);

	arduino->power = 0;
	arduino->power_total = 0;
}

TEST_F(FsTest, Ds2450PollNotifiesPlugins) {
	ow.update_device(0, "20.0200F8FE66771E");
	ow.update_data();
	// poll() reads the ADC, which needs dev->ds - only begin() assigns it
	ow.begin();
	ds2450* dev = (ds2450*)ow.find(0x200200F8FE66771E);
	ASSERT_NE(dev, nullptr);

	// the test fixture plugin counts ACT_DEV_CHANGE and keeps the payload
	plugins.remove("faulty");
	plugins.load(json{ { "faulty", { { "mode", "record" } } } });
	ASSERT_EQ(plugins.config_of("faulty")["changes"], 0);

	LogLevel lvl = logger.get_level();
	// the simulated bus fails the CRC checks, which is only logged
	logger.set_level(LogLevel::NONE);

	// without I2C the ADC always reads 0, so start from something else
	dev->volt_a = 3;
	EXPECT_EQ(dev->poll(), 1);
	json cfg = plugins.config_of("faulty");
	EXPECT_EQ(cfg["changes"], 1);
	EXPECT_EQ(cfg["last"]["rom"], dev->rom);
	EXPECT_EQ(cfg["last"]["type"], "ds2450");
	EXPECT_EQ(cfg["last"]["bus"], 0);
	EXPECT_EQ(cfg["last"]["volt_a"], 0);
	EXPECT_EQ(dev->volt_a, 0);

	// unchanged value: no further notification
	EXPECT_EQ(dev->poll(), 1);
	EXPECT_EQ(plugins.config_of("faulty")["changes"], 1);

	logger.set_level(lvl);
	plugins.remove("faulty");
}

TEST_F(FsTest, Ds2408WriteCfg) {
	char buf[128];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// cfg_write() needs dev->ds; only begin() assigns it
	ow.begin();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);
	std::memset(dev->cfg, 0, DS2408_CFG_SIZE);

	// hex bytes, any white space, upper or lower case, one or two digits
	const char* in = "a5 1 \n21  FF\n";
	res = fs_ops.write("/29.0701F8FE6677F4/cfg", in, strlen(in), 0, nullptr);
	EXPECT_EQ(res, (int)strlen(in));
	EXPECT_EQ(dev->cfg[0], 0xA5);
	EXPECT_EQ(dev->cfg[1], 0x01);
	EXPECT_EQ(dev->cfg[2], 0x21);
	EXPECT_EQ(dev->cfg[3], 0xFF);
	// the rest is left alone
	EXPECT_EQ(dev->cfg[4], 0x00);

	res = fs_ops.read("/29.0701F8FE6677F4/cfg", buf, sizeof(buf), 0, nullptr);
	EXPECT_GT(res, 0);
	EXPECT_EQ(std::string(buf).substr(0, 12), "A5 01 21 FF ");

	// what r_cfg() prints can be written back unchanged
	std::string all(buf);
	res = fs_ops.write("/29.0701F8FE6677F4/cfg", all.c_str(), all.size(), 0, nullptr);
	EXPECT_EQ(res, (int)all.size());
	EXPECT_EQ(dev->cfg[3], 0xFF);

	// rejected, and the cached cfg stays as it was
	const char* bad[] = { "", "  \n", "0x10", "100", "1g", "-1", "10,20" };
	for (const char* b : bad) {
		res = fs_ops.write("/29.0701F8FE6677F4/cfg", b, strlen(b), 0, nullptr);
		EXPECT_EQ(res, -EINVAL) << "input '" << b << "'";
	}
	std::string too_many;
	for (int i = 0; i <= DS2408_CFG_SIZE; i++)
		too_many += "01 ";
	res = fs_ops.write("/29.0701F8FE6677F4/cfg", too_many.c_str(), too_many.size(), 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	EXPECT_EQ(dev->cfg[0], 0xA5);
	EXPECT_EQ(dev->cfg[1], 0x01);
}

TEST_F(FsTest, Ds2408PinNames) {
	char buf[64];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);
	// start from the defaults whatever earlier tests wrote
	for (int i = 0; i < 8; i++) {
		std::string path = "/29.0701F8FE6677F4/pin." + std::to_string(i) + "/name";
		fs_ops.write(path.c_str(), "", 0, 0, nullptr);
	}

	res = fs_ops.read("/29.0701F8FE6677F4/pin.3/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 5);
	EXPECT_STREQ(buf, "PIO.3");

	// a shell "echo Kitchen > .../name" ends in a newline, which is dropped
	res = fs_ops.write("/29.0701F8FE6677F4/pin.3/name", "Kitchen\n", 8, 0, nullptr);
	EXPECT_EQ(res, 8);
	res = fs_ops.read("/29.0701F8FE6677F4/pin.3/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 7);
	EXPECT_STREQ(buf, "Kitchen");
	// other pins keep their default, the device its own name
	fs_ops.read("/29.0701F8FE6677F4/pin.4/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_STREQ(buf, "PIO.4");
	EXPECT_NE(dev->name, "Kitchen");

	// longest allowed name, and one too long, which changes nothing
	std::string longest(PIN_NAME_MAX, 'x');
	res = fs_ops.write("/29.0701F8FE6677F4/pin.5/name", longest.c_str(), longest.size(), 0, nullptr);
	EXPECT_EQ(res, PIN_NAME_MAX);
	fs_ops.read("/29.0701F8FE6677F4/pin.5/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(std::string(buf), longest);
	std::string too_long(PIN_NAME_MAX + 1, 'y');
	res = fs_ops.write("/29.0701F8FE6677F4/pin.5/name", too_long.c_str(), too_long.size(), 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	fs_ops.read("/29.0701F8FE6677F4/pin.5/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(std::string(buf), longest);

	// stored with the device config and restored from it
	json j = dev->to_json();
	ASSERT_TRUE(j["pin_names"].is_array());
	EXPECT_EQ(j["pin_names"].size(), 8u);
	EXPECT_EQ(j["pin_names"][3], "Kitchen");
	EXPECT_EQ(j["pin_names"][4], "");
	ds2408 restored;
	restored.from_json(j);
	json r = restored.to_json();
	EXPECT_EQ(r["pin_names"], j["pin_names"]);

	// older configs without pin_names, or a shorter list, load fine
	json old = j;
	old.erase("pin_names");
	ds2408 legacy;
	EXPECT_NO_THROW(legacy.from_json(old));
	EXPECT_EQ(legacy.to_json()["pin_names"][3], "");
	old["pin_names"] = { "Door", "Hall" };
	ds2408 partial;
	EXPECT_NO_THROW(partial.from_json(old));
	EXPECT_EQ(partial.to_json()["pin_names"][1], "Hall");
	EXPECT_EQ(partial.to_json()["pin_names"][2], "");

	// an empty write resets the default name
	res = fs_ops.write("/29.0701F8FE6677F4/pin.3/name", "\n", 1, 0, nullptr);
	EXPECT_EQ(res, 1);
	fs_ops.read("/29.0701F8FE6677F4/pin.3/name", buf, sizeof(buf), 0, nullptr);
	EXPECT_STREQ(buf, "PIO.3");
	fs_ops.write("/29.0701F8FE6677F4/pin.5/name", "", 0, 0, nullptr);
}

// defined in src/ow_devices.cpp, what load() uses per device
extern std::unique_ptr<OwDev> make_device_from_json(const json& j);

// "poll" is common to every device type and has to survive a save and
// reload, whatever the type adds to or overrides in to_json()
TEST_F(FsTest, PollIsStoredForEveryDeviceType) {
	const struct {
		int bus;
		const char* rom;
	} devs[] = {
		{ 1, "29.0701F8FE6677F4" },	// ds2408
		{ 1, "28.0501FAFE6677A0" },	// ds1820
		{ 0, "20.0200F8FE66771E" },	// ds2450
		{ 1, "AD.0900F8FF6677E2" },	// ard_i2c
	};
	char buf[16];
	int res;

	for (const auto& d : devs) {
		ow.update_device(d.bus, d.rom);
		ow.update_data();
		std::string path = std::string("/") + d.rom + "/poll";

		res = fs_ops.write(path.c_str(), "7\n", 2, 0, nullptr);
		EXPECT_EQ(res, 2) << d.rom;
		res = fs_ops.read(path.c_str(), buf, sizeof(buf), 0, nullptr);
		EXPECT_GT(res, 0) << d.rom;
		EXPECT_STREQ(buf, "7") << d.rom;

		OwDev* dev = ow.find(d.rom);
		ASSERT_NE(dev, nullptr) << d.rom;
		json j = dev->to_json();
		EXPECT_EQ(j.value("poll", -1), 7) << d.rom << " " << j.dump();

		auto restored = make_device_from_json(j);
		ASSERT_NE(restored, nullptr) << d.rom;
		EXPECT_EQ(restored->to_json().value("poll", -1), 7) << d.rom;

		fs_ops.write(path.c_str(), "0\n", 2, 0, nullptr);
	}
}

TEST_F(FsTest, Ds2408Level) {
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};
	char buf[16];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// level_set() goes over the (simulated) bus, which needs dev->ds
	ow.begin();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);
	for (int i = 0; i < 8; i++)
		dev->cfg[CFG_PIN_ID + i] = CFG_OUT_LOW;
	dev->cfg[CFG_PIN_ID + 3] = CFG_OUT_PWM;

	// listed only for the PWM pin
	fs_ops.readdir("/29.0701F8FE6677F4", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "level.3"), seen.end());
	for (int i : { 0, 1, 2, 4, 5, 6, 7 })
		EXPECT_EQ(std::find(seen.begin(), seen.end(), "level." + std::to_string(i)), seen.end()) << i;

	struct stat st;
	EXPECT_EQ(fs_ops.getattr("/29.0701F8FE6677F4/level.3", &st, nullptr), 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));

	// starts at 0, takes 0..100
	res = fs_ops.read("/29.0701F8FE6677F4/level.3", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 1);
	EXPECT_STREQ(buf, "0");
	for (const char* v : { "100", "50", "1", "0" }) {
		std::string in = std::string(v) + "\n";
		res = fs_ops.write("/29.0701F8FE6677F4/level.3", in.c_str(), in.size(), 0, nullptr);
		EXPECT_EQ(res, (int)in.size()) << v;
		res = fs_ops.read("/29.0701F8FE6677F4/level.3", buf, sizeof(buf), 0, nullptr);
		EXPECT_STREQ(buf, v);
	}

	// out of range or not a number: rejected, value unchanged
	fs_ops.write("/29.0701F8FE6677F4/level.3", "42", 2, 0, nullptr);
	for (const char* v : { "101", "-1", "abc", "" }) {
		res = fs_ops.write("/29.0701F8FE6677F4/level.3", v, strlen(v), 0, nullptr);
		EXPECT_EQ(res, -EINVAL) << "'" << v << "'";
	}
	fs_ops.read("/29.0701F8FE6677F4/level.3", buf, sizeof(buf), 0, nullptr);
	EXPECT_STREQ(buf, "42");

	// the switch handler goes through pin_switch(), not level.*: the
	// level it applies has to be what level.N and PIO.N read back
	auto level3 = [&]() {
		fs_ops.read("/29.0701F8FE6677F4/level.3", buf, sizeof(buf), 0, nullptr);
		std::string l(buf);
		fs_ops.read("/29.0701F8FE6677F4/PIO.3", buf, sizeof(buf), 0, nullptr);
		EXPECT_EQ(l, std::string(buf)) << "level.3 and PIO.3 disagree";
		return l;
	};
	EXPECT_EQ(dev->pin_switch(3, OFF), 0);
	EXPECT_EQ(level3(), "0");
	EXPECT_EQ(dev->pin_switch(3, ON), 0);	// no level given: full on
	EXPECT_EQ(level3(), "100");
	EXPECT_EQ(dev->pin_switch(3, TOGGLE), 0);
	EXPECT_EQ(level3(), "0");
	EXPECT_EQ(dev->pin_switch(3, TOGGLE), 0);
	EXPECT_EQ(level3(), "100");
	EXPECT_EQ(dev->pin_switch(3, ON, 30), 0);
	EXPECT_EQ(level3(), "30");
	// and PIO.N on a PWM pin is the same level
	fs_ops.write("/29.0701F8FE6677F4/PIO.3", "70", 2, 0, nullptr);
	EXPECT_EQ(level3(), "70");

	// not a PWM pin: neither readable nor writable
	EXPECT_EQ(fs_ops.read("/29.0701F8FE6677F4/level.2", buf, sizeof(buf), 0, nullptr), -ENOENT);
	EXPECT_EQ(fs_ops.write("/29.0701F8FE6677F4/level.2", "50", 2, 0, nullptr), -ENOENT);
}

TEST_F(FsTest, Ds2408AlarmLatches) {
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};
	auto in_alarm = [&](const std::string& rom) {
		seen.clear();
		fs_ops.readdir("/alarm", &seen, record, 0, nullptr, FUSE_READDIR_PLUS);
		return std::find(seen.begin(), seen.end(), rom) != seen.end();
	};
	auto latched = [&](int i) {
		char buf[8];
		std::string path = "/29.0701F8FE6677F4/latched." + std::to_string(i);
		fs_ops.read(path.c_str(), buf, sizeof(buf), 0, nullptr);
		return std::string(buf);
	};
	uint8_t adr[8] = { 0x29, 0x07, 0x01, 0xF8, 0xFE, 0x66, 0x77, 0xF4 };

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// the alarm read goes over the (simulated) bus, which needs dev->ds,
	// and dev_alarm() logs to the switch handler's bus
	ow.begin();
	swHdl.begin(&ds);
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);
	fs_ops.write("/29.0701F8FE6677F4/latched.0", "0", 1, 0, nullptr);
	EXPECT_FALSE(in_alarm(dev->rom));

	// an alarm: what the register read leaves in data[PIO_LATCH] is kept
	// (the simulated bus reads nothing, so preset it)
	dev->data[STAT] = 0;
	dev->data[PIO_LATCH] = 0x05;
	swHdl.dev_alarm(1, adr);
	EXPECT_TRUE(dev->alarm);
	EXPECT_TRUE(in_alarm(dev->rom));
	// the read reset the latch on the device, data[] mirrors that; the
	// latches stay available through latched.* only
	EXPECT_EQ(dev->data[PIO_LATCH], 0);
	EXPECT_EQ(latched(0), "1");
	EXPECT_EQ(latched(1), "0");
	EXPECT_EQ(latched(2), "1");

	// any other register read (e.g. an uncached BYTE) does not touch it
	dev->data[PIO_LATCH] = 0;
	EXPECT_EQ(latched(0), "1");
	// a second alarm before clearing adds to it
	dev->data[PIO_LATCH] = 0x02;
	swHdl.dev_alarm(1, adr);
	EXPECT_EQ(latched(0), "1");
	EXPECT_EQ(latched(1), "1");
	// an invalid latch read (0xff) is not taken over
	dev->data[PIO_LATCH] = 0xff;
	swHdl.dev_alarm(1, adr);
	EXPECT_EQ(latched(3), "0");

	// only 0 clears, anything else is rejected and changes nothing
	for (const char* v : { "1", "", "x" })
		EXPECT_EQ(fs_ops.write("/29.0701F8FE6677F4/latched.5", v, strlen(v), 0, nullptr), -EINVAL) << v;
	EXPECT_TRUE(in_alarm(dev->rom));
	EXPECT_EQ(latched(0), "1");

	// below /alarm the device is the same as in the root: a directory
	// with its own entries (not the alarm list again), readable files
	std::string adir = "/alarm/" + dev->rom;
	struct stat st;
	EXPECT_EQ(fs_ops.getattr(adir.c_str(), &st, nullptr), 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
	seen.clear();
	EXPECT_EQ(fs_ops.readdir(adir.c_str(), &seen, record, 0, nullptr, FUSE_READDIR_PLUS), 0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "BYTE"), seen.end());
	EXPECT_EQ(std::find(seen.begin(), seen.end(), dev->rom), seen.end());
	for (const auto& name : seen) {
		std::string p = adir + "/" + name;
		EXPECT_EQ(fs_ops.getattr(p.c_str(), &st, nullptr), 0) << p;
	}
	std::string alatch = adir + "/latched.0";
	char buf[8];
	EXPECT_EQ(fs_ops.open(alatch.c_str(), nullptr), 0);
	EXPECT_EQ(fs_ops.read(alatch.c_str(), buf, sizeof(buf), 0, nullptr), 1);
	EXPECT_STREQ(buf, "1");
	// "/alarm/" itself is still the alarm list
	seen.clear();
	fs_ops.readdir("/alarm/", &seen, record, 0, nullptr, FUSE_READDIR_PLUS);
	EXPECT_NE(std::find(seen.begin(), seen.end(), dev->rom), seen.end());

	// 0 to any latch, here through the /alarm path, clears all of them
	// and the alarm
	std::string alatch5 = adir + "/latched.5";
	EXPECT_EQ(fs_ops.write(alatch5.c_str(), "0\n", 2, 0, nullptr), 2);
	EXPECT_FALSE(dev->alarm);
	EXPECT_FALSE(in_alarm(dev->rom));
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(latched(i), "0") << i;
}

// A device change raised while the device is locked reaches the plugins
// only once the (outermost) lock is released, see OwDev::notify_change()
TEST_F(FsTest, DeviceChangeSentAfterUnlock) {
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	ow.begin();
	ds2408* dev = (ds2408*)ow.find(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);
	dev->data[PIO_OUT] = 0xff;

	plugins.remove("faulty");
	plugins.load(json{ { "faulty", { { "mode", "record" } } } });
	{
		auto outer = dev->lock();
		{
			auto inner = dev->lock();
			EXPECT_EQ(dev->pio_set(0x0f), 0xAA);
		}
		// inner released, outer still held: nothing sent yet
		EXPECT_EQ(plugins.config_of("faulty")["changes"], 0);
	}
	json cfg = plugins.config_of("faulty");
	EXPECT_EQ(cfg["changes"], 1);
	EXPECT_EQ(cfg["last"]["pio"], 0x0f);

	// without an outer lock it goes out straight away
	EXPECT_EQ(dev->pio_set(0xf0), 0xAA);
	EXPECT_EQ(plugins.config_of("faulty")["changes"], 2);
	plugins.remove("faulty");
}

TEST_F(FsTest, Ds1820Cfg) {
	std::vector<std::string> seen;
	auto record = [](void* buf, const char* name, const struct stat*,
					 off_t, enum fuse_fill_dir_flags) {
		static_cast<std::vector<std::string>*>(buf)->push_back(name);
		return 0;
	};
	char buf[128];
	int res;
	struct stat st;
	// all of what reading cfg prints: DS1820_CFG_SIZE times "XX "
	auto cfg_str = [&]() {
		buf[0] = '\0';
		fs_ops.read("/28.0501FAFE6677A0/cfg", buf, sizeof(buf), 0, nullptr);
		return std::string(buf);
	};
	static_assert(DS1820_CFG_SIZE == 8, "the expected strings below are 8 bytes");

	ow.update_device(1, "28.0501FAFE6677A0");
	ow.update_data();
	// cfg_read()/cfg_write() go over the (simulated) bus, needs dev->ds
	ow.begin();
	ds1820* dev = (ds1820*)ow.find(1, 5, 0x28);
	ASSERT_NE(dev, nullptr);

	// listed, a regular file
	fs_ops.readdir("/28.0501FAFE6677A0", &seen, record, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_NE(std::find(seen.begin(), seen.end(), "cfg"), seen.end());
	EXPECT_EQ(fs_ops.getattr("/28.0501FAFE6677A0/cfg", &st, nullptr), 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 3 * DS1820_CFG_SIZE);

	// starts out all 0, one "XX " per byte
	res = fs_ops.read("/28.0501FAFE6677A0/cfg", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 3 * DS1820_CFG_SIZE);
	EXPECT_EQ(cfg_str(), "00 00 00 00 00 00 00 00 ");

	// hex bytes, any white space, upper or lower case, one or two digits
	const char* in = "a5 1 \n21  FF\n";
	res = fs_ops.write("/28.0501FAFE6677A0/cfg", in, strlen(in), 0, nullptr);
	EXPECT_EQ(res, (int)strlen(in));
	// the rest is left alone
	EXPECT_EQ(cfg_str(), "A5 01 21 FF 00 00 00 00 ");

	// what reading prints can be written back unchanged
	fs_ops.read("/28.0501FAFE6677A0/cfg", buf, sizeof(buf), 0, nullptr);
	std::string all(buf);
	res = fs_ops.write("/28.0501FAFE6677A0/cfg", all.c_str(), all.size(), 0, nullptr);
	EXPECT_EQ(res, (int)all.size());
	EXPECT_EQ(cfg_str(), "A5 01 21 FF 00 00 00 00 ");

	// rejected, and the cached cfg stays as it was
	const char* bad[] = { "", "  \n", "0x10", "100", "1g", "-1", "10,20" };
	for (const char* b : bad) {
		res = fs_ops.write("/28.0501FAFE6677A0/cfg", b, strlen(b), 0, nullptr);
		EXPECT_EQ(res, -EINVAL) << "input '" << b << "'";
	}
	// exactly DS1820_CFG_SIZE bytes are taken
	const char* full = "1 2 3 4 5 6 7 8";
	res = fs_ops.write("/28.0501FAFE6677A0/cfg", full, strlen(full), 0, nullptr);
	EXPECT_EQ(res, (int)strlen(full));
	EXPECT_EQ(cfg_str(), "01 02 03 04 05 06 07 08 ");
	fs_ops.write("/28.0501FAFE6677A0/cfg", "a5 1 21 ff 0 0 0 0", 18, 0, nullptr);
	std::string too_many;
	for (int i = 0; i <= DS1820_CFG_SIZE; i++)
		too_many += "01 ";
	res = fs_ops.write("/28.0501FAFE6677A0/cfg", too_many.c_str(), too_many.size(), 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	EXPECT_EQ(cfg_str(), "A5 01 21 FF 00 00 00 00 ");

	// the uncached read goes through cfg_read() (the simulated bus reads
	// nothing, so the cache is unchanged)
	res = fs_ops.read("/uncached/28.0501FAFE6677A0/cfg", buf, sizeof(buf), 0, nullptr);
	EXPECT_EQ(res, 3 * DS1820_CFG_SIZE);
	EXPECT_EQ(cfg_str(), "A5 01 21 FF 00 00 00 00 ");

	// directly: cfg_read() reports the size, cfg_write() takes over what
	// it wrote into the cache, longer input is cut to DS1820_CFG_SIZE
	EXPECT_EQ(dev->cfg_read(), DS1820_CFG_SIZE);
	uint8_t data[DS1820_CFG_SIZE + 4] = { 0x11, 0x22, 0x33 };
	EXPECT_EQ(dev->cfg_write(data, 3), 3);
	EXPECT_EQ(cfg_str(), "11 22 33 FF 00 00 00 00 ");
	EXPECT_EQ(dev->cfg_write(data, DS1820_CFG_SIZE + 4), DS1820_CFG_SIZE);
	EXPECT_EQ(cfg_str(), "11 22 33 00 00 00 00 00 ");

	// leave it zeroed for other tests
	std::memset(data, 0, sizeof(data));
	dev->cfg_write(data, DS1820_CFG_SIZE);
}

// get_name()/get_type() hand out copies: a rename from another thread
// cannot invalidate what a plugin already got
TEST_F(FsTest, DevNameAndTypeAreCopies) {
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	IDev* dev = ow.get_dev(0x290701F8FE6677F4);
	ASSERT_NE(dev, nullptr);

	fs_ops.write("/29.0701F8FE6677F4/name", "kitchen\n", 8, 0, nullptr);
	std::string before = dev->get_name();
	EXPECT_EQ(before, "kitchen");
	EXPECT_EQ(dev->get_type(), "ds2408");

	fs_ops.write("/29.0701F8FE6677F4/name", "a much longer hallway name\n", 27, 0, nullptr);
	EXPECT_EQ(before, "kitchen");
	EXPECT_EQ(dev->get_name(), "a much longer hallway name");
	fs_ops.write("/29.0701F8FE6677F4/name", "\n", 1, 0, nullptr);
}
