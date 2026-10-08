#include <fstream>
#include <iostream>
#include <string>
#include <fuse3/fuse.h>
#include "main.h"
#include "plugins.h"
#include "fs.h"
#include "ow_devices.h"
#include "ds2450.h"

extern Plugins plugins;

// Pio.a-d and memory below are documented but were never implemented
// (no fs_read/fs_write case ever handled them); left undisturbed here.
/*
├── volt.A                   (r)  Alias shortcut to read Channel A (0-5.10V scale)
├── volt.B                   (r)  Alias shortcut to read Channel B (0-5.10V scale)
├── volt.C                   (r)  Alias shortcut to read Channel C (0-5.10V scale)
├── volt.D                   (r)  Alias shortcut to read Channel D (0-5.10V scale)
├── volt.ALL                 (r)  Comma-separated list of all channel voltages
|
├── Pio.a                    (rw) Digital output control for pin A (0 or 1)
├── Pio.b                    (rw) Digital output control for pin B (0 or 1)
├── Pio.c                    (rw) Digital output control for pin C (0 or 1)
├── Pio.d                    (rw) Digital output control for pin D (0 or 1)
├── Pio.all                  (rw) Comma-separated output control for all pins
|
├── memory                   (rw) Raw binary dump of internal 32-byte chip memory

*/
// out-of-line definition of the private static members declared in
// ds2450.h; this counts as class scope for access control, so it can
// take the address of the private r_volt() handler directly
const FsEntry<ds2450> ds2450::table[] = {
	{ "volt.*", 4, 4, /* alpha */ true, nullptr, nullptr, &ds2450::r_volt, nullptr },
};
const size_t ds2450::n_table = sizeof(ds2450::table) / sizeof(ds2450::table[0]);

std::vector<std::string> ds2450::fs_dir(string& path) const
{
	auto lk = lock();
	std::vector<std::string> dir = OwDev::fs_dir(path);
	std::vector<std::string> extra = fs_table::dir(*this, table, n_table, path);
	dir.insert(dir.end(), extra.begin(), extra.end());
	return dir;
}

int ds2450::fs_attr(std::string& path) const
{
	auto lk = lock();
	int r = fs_table::attr(*this, table, n_table, path);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_attr(path);
}

// The fs_table.h handlers below are only called from fs_read()/
// fs_write()/fs_dir(), which already hold the device lock. They take it
// again (it is recursive) because static analysis cannot follow the call
// through the table's member pointers and would see unguarded access.

// only ever called with idx 0..3 (A..D), once fs_table.h has already
// matched "volt.*" in path
int ds2450::r_volt(char* buf, size_t size, bool uncached, int idx)
{
	auto lk = lock();
	if (size < 5)
		return -EINVAL;
	return fs_read_volt((uint8_t)idx, buf, uncached);
}

// Converts and reads channel ch from the device and stores the result
// in volts in volt_a..volt_d. Returns 0, or EAGAIN if the conversion
// could not be started.
int ds2450::volt_update(uint8_t ch)
{
	uint16_t volt;
	float volt_x;

	if (adc_read(ch, 0) != 0)
		return EAGAIN;
	volt = adc_read(ch, 1);
	logger.verbose ("DS2450 reading raw=" + std::to_string(volt));
	volt_x = (volt * 5.0 / 1024);
	switch (ch) {
		case 0: volt_a = volt_x; break;
		case 1: volt_b = volt_x; break;
		case 2: volt_c = volt_x; break;
		case 3: volt_d = volt_x; break;
	}
	return 0;
}

int ds2450::fs_read_volt(uint8_t ch, char* buf, bool uncached)
{
	if (uncached) {
		int r = volt_update(ch);
		if (r != 0)
			return r;
	}
	std::sprintf(buf, "%1.2f", adc_get(ch));

	return std::strlen(buf);
}

int ds2450::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	auto lk = lock();
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_read(path, buf, size, uncached);
}

int16_t ds2450::adc_read(uint8_t ch, uint8_t flag)
{
	auto lk = lock();
	if (ch > 3)
		return -1;
	if (flag == 2)
		return dat[ch];

	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction
	std::lock_guard<std::mutex> lock(ds->mtx);
	// coverity[sleep] - bus mutex must be held
	if (!ds->selectChannel(bus))
		return -1;
	if (!ds->reset())
		return -1;
	ds->select(addr);
	if (flag == 0) {
		uint8_t cmd[3] = { 0x3c, (uint8_t)(ch + 1), 0x55 };
		ds->write(cmd[0]);
		ds->write(cmd[1]);
		// clear all
		ds->write(cmd[2]);
		uint8_t inv_crc[2];
		inv_crc[0] = ds->read();
		inv_crc[1] = ds->read();
		// logged, not treated as fatal: the exact CRC byte range for
		// this command isn't verified against real hardware here
		if (!ds->check_crc16(cmd, sizeof(cmd), inv_crc, 0))
			logger.warn(rom + ": DS2450 convert command CRC mismatch");
		return 0;
	}
	uint8_t cmd[3] = { 0xAA, 0x0, 0x0 };
	ds->write(cmd[0]);
	ds->write(cmd[1]);
	ds->write(cmd[2]);
	uint8_t crc_buf[3 + 4 * 2];
	crc_buf[0] = cmd[0];
	crc_buf[1] = cmd[1];
	crc_buf[2] = cmd[2];
	for (int i = 0; i < 4; i++) {
		uint8_t lo = ds->read();
		uint8_t hi = ds->read();
		dat[i] = lo | (hi << 8);
		crc_buf[3 + i * 2] = lo;
		crc_buf[3 + i * 2 + 1] = hi;
	}
	uint8_t inv_crc[2];
	inv_crc[0] = ds->read();
	inv_crc[1] = ds->read();
	if (!ds->check_crc16(crc_buf, sizeof(crc_buf), inv_crc, 0))
		logger.warn(rom + ": DS2450 read result CRC mismatch");

	return dat[ch];
}

float ds2450::adc_get(uint8_t ch)
{
	auto lk = lock();
	switch (ch) {
		case 0: return volt_a;
		case 1: return volt_b;
		case 2: return volt_c;
		case 3: return volt_d;
		default: return -1;
	}
}

// Only ever called once poll_check() has just returned 1.
int ds2450::poll()
{
	auto lk = lock();
	float prev = volt_a;
	if (volt_update(0) != 0)
		return EAGAIN;
	if (volt_a != prev) {
		// sent once the device is unlocked, see OwDev::notify_change()
		notify_change(json{
			{"bus", bus},
			{"type", type},
			{"rom", rom.c_str()},
			{"volt_a", volt_a}
		});
	}
	return 1;
}
