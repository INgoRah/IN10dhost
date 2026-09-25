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
#include "ard_i2c.h"
#include "plugins.h"

extern OwDevices ow;
extern DS2482 ds;
extern Plugins plugins;
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
	EXPECT_EQ(dev->temp_read(2), 0);
}

TEST_F(FsTest, GetDS2450Devices) {
	char buf[1024];
	int res;
	struct stat st;

	ow.update_device(0, "20.0200F8FE66771E");
	ow.update_data();
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
	// not yet supported, should be n.0
	//EXPECT_STREQ(buf, "te");

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
	EXPECT_EQ(res, 2);
	// interpret as hex
	EXPECT_STREQ(buf, "21");
}

TEST_F(FsTest, WriteReadDevPio) {
	//LogLevel lvl = logger.get_level();
	char buf[128];
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();

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
		EXPECT_STREQ(buf, "1");
		dev->data[PIO_OUT] &= ~(0x1 << i);
		res = fs_ops.read(path.c_str(), buf, 2, 0, nullptr);
		EXPECT_EQ(res, 1);
		EXPECT_STREQ(buf, "0");
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
	EXPECT_EQ(buf[0], '1');

	buf[0] = '1';
	buf[1] = '\0';

	// set PIO.0, unset PIO.1 -> byte = 0x1
	fs_ops.write("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '1');
	fs_ops.read("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '0');
	// set PIO.1, (PIO.0 unchanged) -> byte = 0x3
	buf[0] = '1';
	fs_ops.write("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '3');
	fs_ops.read("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '1');
	fs_ops.read("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '1');

	buf[0] = '0';
	fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_EQ(buf[0], '2');
	buf[0] = '0';
	fs_ops.write("/29.0701F8FE6677F4/PIO.1", buf, 2, 0, nullptr);
	res = fs_ops.read("/29.0701F8FE6677F4/BYTE", buf, 2, 0, nullptr);
	EXPECT_STREQ(buf, "0");
	dev->data[PIO_OUT] = 222;
	res = fs_ops.read("/uncached/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "222");
}

TEST_F(FsTest, Ds2408Gaps) {
	int res;
	struct stat st;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
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

	// never exercised: writes the device's cfg block back over the bus
	EXPECT_EQ(dev->cfg_write(), CFG_SIZE);

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