#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <vector>
#include <gtest/gtest.h>
#include "fw_update.h"
#include "fw_fake.h"

/* an Intel HEX line: count, address, type, data and the checksum */
static std::string hex_rec(uint16_t addr, uint8_t type, const std::vector<uint8_t>& data)
{
	uint8_t sum = (uint8_t)(data.size() + (addr >> 8) + (addr & 0xff) + type);
	std::string s = std::format(":{:02X}{:04X}{:02X}", data.size(), addr, type);

	for (uint8_t b : data) {
		s += std::format("{:02X}", b);
		sum += b;
	}
	return s + std::format("{:02X}\n", (uint8_t)-sum);
}

TEST(FwHex, Parse) {
	std::vector<uint8_t> image;
	std::string err;
	// two records with a gap, the gap is 0xff
	std::string text = hex_rec(0, 0, { 0x0c, 0x94, 0x34, 0x00 })
		+ hex_rec(0x10, 0, { 0x11, 0x22 })
		+ hex_rec(0, 3, { 0, 0, 0, 0 })
		+ hex_rec(0, 1, {});

	ASSERT_EQ(fw_parse_hex(text, image, err), 0) << err;
	ASSERT_EQ(image.size(), 0x12u);
	EXPECT_EQ(image[0], 0x0c);
	EXPECT_EQ(image[3], 0x00);
	EXPECT_EQ(image[4], 0xff);
	EXPECT_EQ(image[0x0f], 0xff);
	EXPECT_EQ(image[0x11], 0x22);

	// an extended linear address moves the records after it
	text = hex_rec(0, 4, { 0x00, 0x01 }) + hex_rec(0x0002, 0, { 0xab }) + hex_rec(0, 1, {});
	ASSERT_EQ(fw_parse_hex(text, image, err), 0) << err;
	ASSERT_EQ(image.size(), 0x10003u);
	EXPECT_EQ(image[0x10002], 0xab);
	// a segment address (type 2) is shifted by 4, empty lines are skipped
	text = hex_rec(0, 2, { 0x10, 0x00 }) + "\n" + hex_rec(0x0001, 0, { 0xcd }) + "\n" + hex_rec(0, 1, {});
	ASSERT_EQ(fw_parse_hex(text, image, err), 0) << err;
	ASSERT_EQ(image.size(), 0x10002u);
	EXPECT_EQ(image[0x10001], 0xcd);
	// CR LF line ends, as from Windows
	text = hex_rec(0, 0, { 0x01 });
	text.insert(text.size() - 1, "\r");
	ASSERT_EQ(fw_parse_hex(text + hex_rec(0, 1, {}), image, err), 0) << err;
}

TEST(FwHex, Errors) {
	std::vector<uint8_t> image;
	std::string err;
	const std::string end = hex_rec(0, 1, {});
	std::string bad_sum = hex_rec(0, 0, { 1, 2 });
	bad_sum[bad_sum.size() - 2] ^= 1;

	EXPECT_EQ(fw_parse_hex(bad_sum + end, image, err), -EINVAL);
	EXPECT_NE(err.find("line 1: checksum"), std::string::npos) << err;
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 0, { 1 }), image, err), -EINVAL);
	EXPECT_NE(err.find("end of file"), std::string::npos) << err;
	EXPECT_EQ(fw_parse_hex(":0100000001\n" + end, image, err), -EINVAL) << "length";
	EXPECT_EQ(fw_parse_hex(":01000000ZZFF\n" + end, image, err), -EINVAL) << "digits";
	EXPECT_EQ(fw_parse_hex("hello\n" + end, image, err), -EINVAL);
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 7, {}) + end, image, err), -EINVAL);
	EXPECT_NE(err.find("type 0x07"), std::string::npos) << err;
	EXPECT_EQ(fw_parse_hex(end, image, err), -EINVAL);
	EXPECT_EQ(err, "no data");
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 4, { 0x01, 0x00 }) + hex_rec(0, 0, { 1 }) + end, image, err),
		-EFBIG);
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 2, { 0x10 }) + end, image, err), -EINVAL);
	EXPECT_NE(err.find("segment address wrong"), std::string::npos) << err;
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 4, { 0, 1, 2 }) + end, image, err), -EINVAL);
	EXPECT_NE(err.find("linear address wrong"), std::string::npos) << err;
	// at + len would wrap around with a 32 bit size_t
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 4, { 0xff, 0xff }) + hex_rec(0xffff, 0, { 1, 2 }) + end,
		image, err), -EFBIG);
	EXPECT_NE(err.find("too large"), std::string::npos) << err;
	// the last byte still fits, one more does not
	std::vector<uint8_t> page(16, 0x55);
	std::string top = hex_rec(0, 4, { 0x00, 0x03 }) + hex_rec(0xfff0, 0, page);
	ASSERT_EQ(fw_parse_hex(top + end, image, err), 0) << err;
	EXPECT_EQ(image.size(), 256u * 1024);
	top = hex_rec(0, 4, { 0x00, 0x03 }) + hex_rec(0xfff1, 0, page);
	EXPECT_EQ(fw_parse_hex(top + end, image, err), -EFBIG);
	// nothing after the end
	EXPECT_EQ(fw_parse_hex(hex_rec(0, 0, { 1 }) + end + hex_rec(1, 0, { 2 }), image, err), -EINVAL);
}

TEST(FwHex, Load) {
	namespace fs = std::filesystem;
	fs::path dir = fs::temp_directory_path() / std::format("in10dfs-fw-{}", getpid());
	std::vector<uint8_t> image;
	std::string err;

	fs::create_directories(dir);
	std::ofstream(dir / "fw.hex") << hex_rec(0, 0, { 0xaa, 0xbb }) << hex_rec(0, 1, {});
	ASSERT_EQ(fw_load((dir / "fw.hex").string(), image, err), 0) << err;
	EXPECT_EQ(image, (std::vector<uint8_t>{ 0xaa, 0xbb }));
	{
		std::ofstream b(dir / "fw.bin", std::ios::binary);
		b.write("\x0c\x94\x00", 3);
	}
	ASSERT_EQ(fw_load((dir / "fw.bin").string(), image, err), 0) << err;
	EXPECT_EQ(image, (std::vector<uint8_t>{ 0x0c, 0x94, 0x00 }));
	{
		std::ofstream b(dir / "big.bin", std::ios::binary);
		b << std::string(256 * 1024 + 1, '\x55');
	}
	EXPECT_EQ(fw_load((dir / "big.bin").string(), image, err), -EFBIG);
	EXPECT_NE(err.find("too large"), std::string::npos) << err;
	std::ofstream(dir / "empty.bin");
	EXPECT_EQ(fw_load((dir / "empty.bin").string(), image, err), -EINVAL);
	EXPECT_EQ(fw_load((dir / "none.hex").string(), image, err), -ENOENT);
	EXPECT_NE(err.find("cannot open"), std::string::npos) << err;
	fs::remove_all(dir);
}

TEST(FwFlash, Update) {
	FakeBl bl;
	std::vector<uint8_t> image(300);
	std::vector<std::pair<int, int>> steps;
	std::string err;

	for (size_t i = 0; i < image.size(); i++)
		image[i] = (uint8_t)(i * 3 + 1);
	ASSERT_EQ(fw_flash(bl, image, [&](int d, int t) { steps.push_back({ d, t }); }, err), 0) << err;
	// page 0 erased first and written last, 1 and 2 in between
	ASSERT_EQ(bl.writes.size(), 4u);
	EXPECT_EQ(bl.writes[0], 0);
	EXPECT_EQ(bl.writes[1], 128);
	EXPECT_EQ(bl.writes[2], 256);
	EXPECT_EQ(bl.writes[3], 0);
	EXPECT_TRUE(bl.erased_first);
	// the image, the rest of the last page 0xff
	EXPECT_EQ(std::memcmp(bl.flash.data(), image.data(), image.size()), 0);
	EXPECT_EQ(bl.flash[300], 0xff);
	EXPECT_EQ(bl.flash[383], 0xff);
	ASSERT_EQ(steps.size(), 5u);
	EXPECT_EQ(steps.front(), std::make_pair(0, 4));
	EXPECT_EQ(steps.back(), std::make_pair(4, 4));
}

TEST(FwFlash, Retries) {
	FakeBl bl;
	std::vector<uint8_t> image(200, 0x5a);
	std::string err;

	// a CRC error, a page read back wrong: tried again
	bl.fail_writes = { 1 };
	bl.corrupt_reads = { 2 };
	ASSERT_EQ(fw_flash(bl, image, nullptr, err), 0) << err;
	EXPECT_EQ(std::memcmp(bl.flash.data(), image.data(), image.size()), 0);
	EXPECT_EQ(bl.writes.size(), 5u);
}

TEST(FwFlash, Errors) {
	std::string err;
	{
		FakeBl bl;
		// a page that never goes: broken off, page 0 stays erased
		bl.fail_writes = { 1, 2, 3 };
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(200, 1), nullptr, err), -EIO);
		EXPECT_NE(err.find("page 0x0080"), std::string::npos) << err;
		EXPECT_EQ(bl.flash[0], 0xff);
	}
	{
		FakeBl bl;
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(bl.flash.size() + 1, 1), nullptr, err), -EFBIG);
		EXPECT_TRUE(bl.writes.empty());
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(), nullptr, err), -EFBIG);
		// nothing at the reset vector
		std::vector<uint8_t> image(300, 0xff);
		image[200] = 1;
		EXPECT_EQ(fw_flash(bl, image, nullptr, err), -EINVAL);
		EXPECT_TRUE(bl.writes.empty());
	}
	{
		// an error code of the device on every try
		FakeBl bl;
		bl.fail_writes = { 1, 2, 3 };
		bl.fail_code = 0x05;
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(200, 1), nullptr, err), -EIO);
		EXPECT_NE(err.find("page 0x0080: error 0x05 of the device"), std::string::npos) << err;
	}
	{
		// verify failed on the device every time
		FakeBl bl;
		bl.fail_writes = { 1, 2, 3 };
		bl.fail_code = BL_ERR_VERIFY;
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(200, 1), nullptr, err), -EIO);
		EXPECT_NE(err.find("page 0x0080: does not read back"), std::string::npos) << err;
	}
	{
		// written fine, but never read back the same
		FakeBl bl;
		bl.corrupt_reads = { 1, 2, 3 };
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(200, 1), nullptr, err), -EIO);
		EXPECT_NE(err.find("page 0x0080: does not read back"), std::string::npos) << err;
	}
	{
		FakeBl bl;
		bl.answers = false;
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(10, 1), nullptr, err), -EIO);
		EXPECT_NE(err.find("no answer of the bootloader"), std::string::npos) << err;
	}
	{
		FakeBl bl;
		bl.refuse = true;
		EXPECT_EQ(fw_flash(bl, std::vector<uint8_t>(10, 1), nullptr, err), -EINVAL);
		EXPECT_NE(err.find("refused"), std::string::npos) << err;
	}
}

TEST(FwInfo, Parse) {
	const uint8_t raw[BL_INFO_SIZE] = { 'B', 'L', 1, 128, 0x00, 0x38, 0x1e, 0x94, 0x06, 1, BL_OK };
	BlInfo info;

	ASSERT_TRUE(bl_info_parse(raw, info));
	EXPECT_EQ(info.version, 1);
	EXPECT_EQ(info.page_size, 128);
	EXPECT_EQ(info.app_size, 0x3800);
	EXPECT_EQ(info.signature[1], 0x94);
	EXPECT_TRUE(info.app_valid);
	uint8_t bad[BL_INFO_SIZE];
	std::memcpy(bad, raw, sizeof(bad));
	bad[1] = 'X';
	EXPECT_FALSE(bl_info_parse(bad, info));
	bad[1] = 'L';
	bad[3] = 100;
	EXPECT_FALSE(bl_info_parse(bad, info)) << "page size";
}
