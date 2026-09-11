#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <fuse3/fuse.h>
#include <algorithm> // for std::max,min...
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "ow_dev.h"

static struct filetype generic[] = {
	{ "id", 3 },
	{ "status", 1 },
	{ "name", 1 },
	{ "info", 1 },
	{ "poll", 5 },
};

OwDev::OwDev(std::string rom)
{
	this->rom = rom;
	poll_interval = 0;
	id = 0;
	update();
}

json OwDev::to_json() const
{
	return json{
		{"type", type},
		{"bus", bus},
		{"rom", rom},
		{"id", id},
		{"name", name},
		{"poll", poll_interval},
	};
}

void OwDev::from_json(const json& j)
{
	j.at("bus").get_to(bus); // TODO throw exception if bus is out of range
	j.at("rom").get_to(rom);
	j.at("id").get_to(id);
	j.at("name").get_to(name);
	j.at("type").get_to(type);
	if (j.contains("poll"))
		j.at("poll").get_to(poll_interval);
}

// Dow-CRC using polynomial X^8 + X^5 + X^4 + X^0
// Tiny 2x16 entry CRC table created by Arjen Lentz
// See http://lentz.com.au/blog/calculating-crc-with-a-tiny-32-entry-lookup-table
static const uint8_t dscrc2x16_table[] = {
	0x00, 0x5E, 0xBC, 0xE2, 0x61, 0x3F, 0xDD, 0x83,
	0xC2, 0x9C, 0x7E, 0x20, 0xA3, 0xFD, 0x1F, 0x41,
	0x00, 0x9D, 0x23, 0xBE, 0x46, 0xDB, 0x65, 0xF8,
	0x8C, 0x11, 0xAF, 0x32, 0xCA, 0x57, 0xE9, 0x74
};

// Compute a Dallas Semiconductor 8 bit CRC. These show up in the ROM
// and the registers.  (Use tiny 2x16 entry CRC table)
uint8_t OwDev::crc8(const uint8_t *addr, uint8_t len)
{
	uint8_t crc = 0;

	while (len--) {
		crc = *addr++ ^ crc;  // just re-using crc as intermediate
		crc = dscrc2x16_table[crc & 0x0f] ^
		dscrc2x16_table[16 + ((crc >> 4) & 0x0f)];
	}

	return crc;
}

void OwDev::update()
{
	int pos;

	// Check if the string is long enough and doesn't already have the dot
	if (rom.length() >= 2 && rom[2] != '.')
		rom.insert(2, ".");
	if (rom.length() >= (3 + 2 * 6))
		rom.erase(3 + 2 * 6, 2);
	addr[0] = std::strtoull(rom.substr(0, 2).c_str(), nullptr, 16);
	pos = 3;
	for (int i = 1; i < 7; i++) {
		addr[i] = std::strtoull(rom.substr(pos, 2).c_str(), nullptr, 16);
		pos += 2;
	}
	addr[7] = crc8 (addr, 7);

	rom = std::format("{}{:02X}", rom, addr[7]);
	string s = rom;
	s.erase(2, 1);
	rom_code = std::strtoull(s.c_str(), nullptr, 16);
	if (id == 0)
		id = addr[1];
}

void OwDev::begin(DS2482 *ds)
{
	update();
	std::lock_guard<std::mutex> lock(ds->mtx);
	ow = ds;
	logger.verbose(std::format("Device id={} type={}, rom={}  ({})  initialized ", id, type, rom, rom_code));
};

void OwDev::set_mode(int mode)
{
	this->mode = mode;
	logger.verbose(std::format("Device rom={} mode={}", rom, mode));
}

int OwDev::poll()
{
	if (poll_interval == 0)
		// no polling
		return -1;
	// check whether it needs polling..
	auto now = HrClock::now();
	if (now - last_poll >= std::chrono::milliseconds(1000 * poll_interval)) {
		// if yes set next poll time
		last_poll = now;
		// TODO do the actual polling action in the sub classes, e.g. read the state and update the cache
		return 1;
	}

	// do nothing
	return 0;
}

int OwDev::poll_next()
{
	if (poll_interval == 0)
		// no polling
		return -1;

	auto now = HrClock::now();
	auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll).count();

	return std::max((int)0, (int)(poll_interval * 1000 - diff));
}

int OwDev::fs_attr(std::string& path) const
{
	if (path.length() == 0)
		return 0;
	for (const auto& s : generic) {
		if (path.find(s.name) != std::string::npos) {
			if (strcmp(s.name, "name") == 0)
				return (std::max(name.length(), (size_t)1));
			return s.suglen;
		}
	}
	// rom level
	return 0;
}

int OwDev::fs_open(std::string& path) const
{
	(void)path;

	return 0;
}

std::vector<std::string> OwDev::fs_dir(string& path) const
{
	std::vector<std::string> dir;
	(void)path;

	for (const auto& s : generic) {
		dir.push_back(s.name);
	}

	return dir;
}

int OwDev::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	(void)uncached;
	logger.debug("Based class read " + type);
	if (path.find("name") != string::npos) {
		std::strncpy(buf, name.c_str(), size);
		//std::sprintf(buf, name.c_str());
		return std::strlen(buf);
	}
	if (path.find("id") != string::npos) {
		std::sprintf(buf, "%d", id);
		return std::strlen(buf);
	}
	if (path.find("poll") != string::npos) {
		std::sprintf(buf, "%d", poll_interval);
		return std::strlen(buf);
	}

	return 0;
}

int OwDev::fs_write(string& path, const char* buf, size_t size)
{
	if (path.find("name") != string::npos) {
		name.assign(buf, size - 1); // Exclude \n terminator
	}
	if (path.find("id") != string::npos) {
		try {
			id = (uint8_t)std::stoi(buf);
		} catch (const std::exception& e) {
			logger.warn("Invalid id value: " + std::string(buf));
			return -1;
		}
	}
	if (path.find("poll") != string::npos) {
		try {
			poll_interval = (uint16_t)std::stoi(buf);
		} catch (const std::exception& e) {
			logger.warn("Invalid poll value: " + std::string(buf));
			return -1;
		}
	}

	return size;
}
