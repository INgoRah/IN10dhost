#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include "fw_update.h"

/* larger than any owSlave flash, against a wrong file */
#define FW_MAX_SIZE (256 * 1024)
/* a page written or read back wrong is tried again so often */
#define FW_PAGE_TRIES 3

bool bl_info_parse(const uint8_t* raw, BlInfo& info)
{
	if (raw[0] != 'B' || raw[1] != 'L')
		return false;
	info.version = raw[2];
	// 256 does not fit the byte, the ATmega168/328 have 128
	info.page_size = raw[3] ? raw[3] : 256;
	info.app_size = raw[4] | (raw[5] << 8);
	std::memcpy(info.signature, raw + 6, 3);
	info.app_valid = raw[9] != 0;
	info.last_result = raw[10];
	// a power of two
	return (info.page_size & (info.page_size - 1)) == 0;
}

static int hex_byte(const std::string& s, size_t pos)
{
	if (pos + 2 > s.size() || !isxdigit((unsigned char)s[pos]) ||
	    !isxdigit((unsigned char)s[pos + 1]))
		return -1;
	return std::stoi(s.substr(pos, 2), nullptr, 16);
}

int fw_parse_hex(const std::string& text, std::vector<uint8_t>& image, std::string& err)
{
	std::istringstream in(text);
	std::string line;
	uint32_t base = 0;
	int n = 0;
	bool end = false;

	image.clear();
	while (std::getline(in, line)) {
		n++;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (line.empty())
			continue;
		if (end || line[0] != ':' || line.size() < 11 || (line.size() - 1) % 2 != 0) {
			err = std::format("line {}: no Intel HEX record", n);
			return -EINVAL;
		}
		std::vector<uint8_t> rec;
		uint8_t sum = 0;
		for (size_t i = 1; i < line.size(); i += 2) {
			int b = hex_byte(line, i);
			if (b < 0) {
				err = std::format("line {}: no hex digits", n);
				return -EINVAL;
			}
			rec.push_back((uint8_t)b);
			sum += (uint8_t)b;
		}
		size_t len = rec[0];
		if (rec.size() != len + 5) {
			err = std::format("line {}: length {} does not match", n, len);
			return -EINVAL;
		}
		if (sum != 0) {
			err = std::format("line {}: checksum wrong", n);
			return -EINVAL;
		}
		uint32_t addr = (rec[1] << 8) | rec[2];
		const uint8_t* data = rec.data() + 4;
		switch (rec[3]) {
			case 0x00: {
				uint32_t at = base + addr;
				// not at + len: that wraps with a 32 bit size_t (ARM)
				// for an address near 4 GB and would pass
				if (at > FW_MAX_SIZE || len > FW_MAX_SIZE - at) {
					err = std::format("line {}: address {:#x} too large", n, at);
					return -EFBIG;
				}
				size_t stop = at + len;
				if (image.size() < stop)
					image.resize(stop, 0xff);
				std::memcpy(image.data() + at, data, len);
				break;
			}
			case 0x01:
				end = true;
				break;
			case 0x02:
				if (len != 2) {
					err = std::format("line {}: segment address wrong", n);
					return -EINVAL;
				}
				base = ((data[0] << 8) | data[1]) << 4;
				break;
			case 0x04:
				if (len != 2) {
					err = std::format("line {}: linear address wrong", n);
					return -EINVAL;
				}
				base = (uint32_t)((data[0] << 8) | data[1]) << 16;
				break;
			case 0x03:
			case 0x05:
				// start address, the AVR starts at its reset vector
				break;
			default:
				err = std::format("line {}: record type {:#04x} unknown", n, rec[3]);
				return -EINVAL;
		}
	}
	if (!end) {
		err = "no end of file record";
		return -EINVAL;
	}
	if (image.empty()) {
		err = "no data";
		return -EINVAL;
	}
	return 0;
}

int fw_load(const std::string& path, std::vector<uint8_t>& image, std::string& err)
{
	std::ifstream f(path, std::ios::binary);

	if (!f) {
		err = "cannot open " + path + ": " + strerror(errno);
		return -ENOENT;
	}
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (!text.empty() && text[0] == ':')
		return fw_parse_hex(text, image, err);
	if (text.size() > FW_MAX_SIZE) {
		err = std::format("{} bytes, too large", text.size());
		return -EFBIG;
	}
	if (text.empty()) {
		err = path + " is empty";
		return -EINVAL;
	}
	image.assign(text.begin(), text.end());
	return 0;
}

/* one page, until it reads back as written */
static int page_write(FwLink& link, uint16_t addr, const uint8_t* data, size_t len,
	std::string& err)
{
	std::vector<uint8_t> back(len);
	int r = 0;

	for (int i = 0; i < FW_PAGE_TRIES; i++) {
		r = link.write_page(addr, data, len);
		if (r == BL_ERR_ADDR) {
			err = std::format("page {:#06x}: refused by the device", addr);
			return -EINVAL;
		}
		if (r != BL_OK)
			continue;
		r = link.read_page(addr, back.data(), len);
		if (r == 0 && std::memcmp(back.data(), data, len) == 0)
			return 0;
		// read back, but not as written
		if (r == 0)
			r = BL_ERR_VERIFY;
	}
	if (r < 0)
		err = std::format("page {:#06x}: {}", addr, strerror(-r));
	else if (r == BL_ERR_VERIFY)
		err = std::format("page {:#06x}: does not read back as written", addr);
	else
		err = std::format("page {:#06x}: error {:#04x} of the device", addr, r);
	return -EIO;
}

int fw_flash(FwLink& link, const std::vector<uint8_t>& image,
	const std::function<void(int, int)>& progress, std::string& err)
{
	BlInfo info;
	int r = link.info(info);

	if (r < 0) {
		err = std::string("no answer of the bootloader: ") + strerror(-r);
		return r;
	}
	if (image.empty() || image.size() > info.app_size) {
		err = std::format("image of {} bytes, the device takes up to {}",
			image.size(), info.app_size);
		return -EFBIG;
	}
	const size_t ps = info.page_size;
	const size_t pages = (image.size() + ps - 1) / ps;
	std::vector<uint8_t> padded(image);
	padded.resize(pages * ps, 0xff);
	if (std::all_of(padded.begin(), padded.begin() + ps, [](uint8_t b) { return b == 0xff; })) {
		err = "the image has no reset vector (page 0 empty)";
		return -EINVAL;
	}
	// +1: page 0 is written twice, erased and at last
	const int total = (int)pages + 1;
	int done = 0;
	std::vector<uint8_t> erased(ps, 0xff);

	if (progress)
		progress(done, total);
	r = page_write(link, 0, erased.data(), ps, err);
	if (r < 0)
		return r;
	if (progress)
		progress(++done, total);
	for (size_t p = 1; p <= pages; p++) {
		// page 0 at last
		size_t page = p == pages ? 0 : p;
		r = page_write(link, (uint16_t)(page * ps), padded.data() + page * ps, ps, err);
		if (r < 0)
			return r;
		if (progress)
			progress(++done, total);
	}
	return 0;
}
