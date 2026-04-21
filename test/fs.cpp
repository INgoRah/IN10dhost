#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"

extern OwDevices ow;
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
	logger.set_level(LogLevel::VERBOSE);
	res = fs_ops.read("/28.0501FAFE6677A0/temperature", buf, 32, 0, nullptr);
	EXPECT_EQ(res, 4);
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
	logger.set_level(LogLevel::WARN);
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
	LogLevel lvl = logger.get_level();
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
	//logger.verbose(std::format("cfg {}: {}", res, std::string(buf, res)));
	//logger.verbose(std::format("cfg {}: {}", res, std::string(buf, res)));
	logger.set_level(lvl);
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
	dev->data[PIO_LS] = 222;
	res = fs_ops.read("/uncached/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_STREQ(buf, "222");
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
	lvl = (int)logger.get_level();
	EXPECT_EQ(lvl, 8);

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
}