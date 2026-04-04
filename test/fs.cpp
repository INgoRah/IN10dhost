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
struct fuse_operations fs_ops = {};


int filler(void *buf, const char *name,
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
	struct stat st;
	// Mock 1 bus available
	//EXPECT_CALL(ow, bus_count()).WillRepeatedly(testing::Return(1));

	int res = fs_ops.getattr("/bus.0", &st, nullptr);

	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// Test device file attribute retrieval
TEST_F(FsTest, GetDS2408Devices) {
	char buf[1024];
	int res;
	struct stat st;
	//std::string rom = "28.123456789012";
	//MockOwDev mock_dev(rom);
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	// TODO: write cfg for pins with 0x21, 0x23, 0x10 and read dir
	res = fs_ops.readdir("/29.0701F8FE6677F4", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);

	//OwDev* dev = ow.find("29.0701F8FE6677F4");

	//EXPECT_CALL(ow, find(testing::_)).WillRepeatedly(testing::Return(&mock_dev));
	// Simulate a file of size 512 bytes
	//EXPECT_CALL(dev, fs_attr(testing::_)).WillOnce(testing::Return(512));
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
	//std::string rom = "28.123456789012";
	//MockOwDev mock_dev(rom);
	ow.update_device(1, "28.0501FAFE6677A0");
	ow.update_data();
	res = fs_ops.readdir("/28.0501FAFE6677A0", buf, filler, 0, nullptr, (enum fuse_readdir_flags)0);
	EXPECT_EQ(res, 0);
	res = fs_ops.getattr("/28.0501FAFE6677A0/temperature", &st, nullptr);
	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISREG(st.st_mode));
	EXPECT_EQ(st.st_size, 5);
}

// Test device file attribute retrieval
TEST_F(FsTest, GetAttrDeviceFile) {
	struct stat st;
	//std::string rom = "28.123456789012";
	//MockOwDev mock_dev(rom);
	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	//OwDev* dev = ow.find("29.0701F8FE6677F4");

	//EXPECT_CALL(ow, find(testing::_)).WillRepeatedly(testing::Return(&mock_dev));
	// Simulate a file of size 512 bytes
	//EXPECT_CALL(dev, fs_attr(testing::_)).WillOnce(testing::Return(512));
	int res = fs_ops.getattr("/29.0701F8FE6677F4/name", &st, nullptr);
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
	//EXPECT_CALL(ow, search(true)).Times(1);

	auto mock_filler = [](void* buf, const char* name, const struct stat* st,
						  off_t off, enum fuse_fill_dir_flags flags) {
		(void)buf; (void)name; (void)st; (void)off; (void)flags;
		return 0;
	};

	fs_ops.readdir("/uncached", nullptr, mock_filler, 0, nullptr, FUSE_READDIR_PLUS);
}

TEST_F(FsTest, WriteReadDev) {
	LogLevel lvl = logger.get_level();
	int res;

	ow.update_device(1, "29.0701F8FE6677F4");
	ow.update_data();
	//logger.set_level(LogLevel::VERBOSE);

	//EXPECT_CALL(ow, find(testing::_)).WillRepeatedly(testing::Return(&mock_dev));
	// Simulate a file of size 512 bytes
	//EXPECT_CALL(dev, fs_attr(testing::_)).WillOnce(testing::Return(512));
	char buf[32];

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
	//logger.set_level(LogLevel::VERBOSE);

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
	EXPECT_EQ(buf[0], '0');
	res = fs_ops.read("/uncached/29.0701F8FE6677F4/BYTE", buf, 3, 0, nullptr);
	EXPECT_EQ(buf[0], '2');
	EXPECT_EQ(buf[1], '2');
	EXPECT_EQ(buf[2], '2');
	logger.set_level(lvl);
}

// Test root directory attributes
TEST_F(FsTest, SettingsDirectoryAttr) {
	struct stat st;
	int res = fs_ops.getattr("/settings", &st, nullptr);

	EXPECT_EQ(res, 0);
	EXPECT_TRUE(S_ISDIR(st.st_mode));
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
	logger.set_level(prev_lvl);
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
