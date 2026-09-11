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
#include "ds1820.h"
#include "ard_i2c.h"

extern OwDevices ow;
extern DS2482 ds;
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
		ow.begin(&ds);
		ow.set_mode(0x10);
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

	ow.update_device(1, "28.0501FAFE6677A0");
	ow.update_data();
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
	res = fs_ops.read("/uncached/28.0501FAFE6677A0/temperature", buf, 32, 0, nullptr);
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
	char buf[128];
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

	// Arduino-relay path (mode != 0x10): the FsTest fixture defaults
	// every new device to 0x10 via OwDevices::set_mode(), so the
	// non-relay ard_set() branch is otherwise never taken
	dev->set_mode(0);
	buf[0] = '1';
	buf[1] = '\0';
	res = fs_ops.write("/29.0701F8FE6677F4/PIO.0", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	dev->set_mode(0x10);

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

	res = fs_ops.getattr("/settings/mode", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	res = fs_ops.open("/settings/mode",  nullptr);
	EXPECT_EQ(res, 0);
	res = fs_ops.read("/settings/mode", buf, 2, 0, nullptr);
	EXPECT_EQ(res, 2);
	buf[0] = '1';
	buf[1] = '6';
	buf[2] = '\0';
	res = fs_ops.write("/settings/mode", buf, 3, 0, nullptr);
	EXPECT_EQ(res, 3);

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
	EXPECT_EQ(arduino->begin(&ow), 0);
	// no-ops without USE_I2C, just exercised for coverage
	arduino->interrupt();
	arduino->events(-1);
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
	// SwitchHandler::fs_open(), which unconditionally succeeds
	int res = fs_ops.open("/switches/list", nullptr);
	EXPECT_EQ(res, 0);
}

TEST_F(FsTest, SettingsWriteErrors) {
	int res;

	res = fs_ops.write("/settings/log", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/settings/mode", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);
	res = fs_ops.write("/settings/poll", (char*)"nope", 4, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// out of uint8_t range as well as non-numeric
	res = fs_ops.write("/settings/mode", (char*)"99999999999", 11, 0, nullptr);
	EXPECT_EQ(res, -EINVAL);

	// triggers Plugins::reload() + action(INITIALIZED); the write then
	// falls through to the generic device/switch handling below, which
	// doesn't recognize this path either
	res = fs_ops.write("/settings/plugins", (char*)"", 0, 0, nullptr);
	EXPECT_EQ(res, -ENOENT);
}