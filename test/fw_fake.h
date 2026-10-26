#ifndef _FW_FAKE_H
#define _FW_FAKE_H
/* A bootloader as owSlave's on an ATmega168, for the firmware update
   tests: its flash in memory, failures to order. */
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <set>
#include <vector>
#include "fw_update.h"

struct FakeBl : public FwLink {
	std::vector<uint8_t> flash = std::vector<uint8_t>(0x3800, 0xff);
	// the address of every write_page() call
	std::vector<int> writes;
	bool erased_first = false;
	// write_page() calls (counted from 0) failing, with fail_code
	std::set<int> fail_writes;
	int fail_code = -EIO;
	// read_page() calls (counted from 0) giving wrong data
	std::set<int> corrupt_reads;
	int reads = 0;
	// no answer to info() at all, or to the first info_fails of them
	bool answers = true;
	int info_fails = 0;
	bool refuse = false;
	bool exited = false;
	// called in each write_page(), e.g. to hold the update
	std::function<void()> on_write;

	int info(BlInfo& info) override
	{
		if (!answers || info_fails-- > 0)
			return -EIO;
		info.version = BL_VERSION;
		info.page_size = 128;
		info.app_size = (uint16_t)flash.size();
		info.app_valid = flash[0] != 0xff || flash[1] != 0xff;
		return 0;
	}

	int write_page(uint16_t addr, const uint8_t* data, size_t len) override
	{
		int n = (int)writes.size();

		if (on_write)
			on_write();
		writes.push_back(addr);
		if (refuse)
			return BL_ERR_ADDR;
		if (fail_writes.count(n))
			return fail_code;
		if (n == 0)
			erased_first = addr == 0 &&
				std::all_of(data, data + len, [](uint8_t b) { return b == 0xff; });
		std::memcpy(flash.data() + addr, data, len);
		return BL_OK;
	}

	int read_page(uint16_t addr, uint8_t* data, size_t len) override
	{
		std::memcpy(data, flash.data() + addr, len);
		if (corrupt_reads.count(reads++))
			data[len / 2] ^= 0x40;
		return 0;
	}

	int exit() override
	{
		if (flash[0] == 0xff && flash[1] == 0xff)
			return -ENOEXEC;
		exited = true;
		return 0;
	}
};
#endif
