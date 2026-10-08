#include <fstream>
#include <iostream>
#include <string>
#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "ds1820.h"

/*
 * Local constants
 */
#define READSCRATCH     0xBE  // Read from scratchpad
#define WRITESCRATCH    0x4E  // Write to scratchpad
#define STARTCONVO      0x44  // Tells device to take a temperature reading and put it on the scratchpad
// Scratchpad locations
#define TEMP_LSB        0
#define TEMP_MSB        1
#define HIGH_ALARM_TEMP 2
#define LOW_ALARM_TEMP  3

// out-of-line definition of the private static members declared in
// ds1820.h; this counts as class scope for access control, so it can
// take the address of the private handlers below directly
const FsEntry<ds1820> ds1820::table[] = {
	{ "temperature", 5, 0, false, nullptr, nullptr, &ds1820::r_temperature, nullptr },
	{ "humidity", 4, 0, false, nullptr, nullptr, &ds1820::r_humidity, nullptr },
	{ "cfg", 3 * DS1820_CFG_SIZE, 0, false, nullptr, nullptr, &ds1820::r_cfg, &ds1820::w_cfg },
};
const size_t ds1820::n_table = sizeof(ds1820::table) / sizeof(ds1820::table[0]);

std::vector<std::string> ds1820::fs_dir(string& path) const
{
	auto lk = lock();
	std::vector<std::string> dir = OwDev::fs_dir(path);
	std::vector<std::string> extra = fs_table::dir(*this, table, n_table, path);
	dir.insert(dir.end(), extra.begin(), extra.end());
	return dir;
}

int ds1820::fs_attr(std::string& path) const
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
int ds1820::r_temperature(char* buf, size_t, bool uncached, int)
{
	auto lk = lock();
	//std::unique_lock<std::mutex> mtx(ds->mtx, std::defer_lock);
	//std::unique_lock<std::mutex> mtx(ds->mtx);
	if (uncached) {
		temp_read(0);
		//logger.info ("#1 reading %s/%s %d ...\n", rom.c_str(), path.c_str(), ret);
		temp_read(1);
		logger.debug("DS1820 reading temp " +  std::to_string(temp) + "° " + std::to_string(hum) + " %");
	}
	alarm = false;
	std::sprintf(buf, "%1.2f", temp);
	return std::strlen(buf);
}

int ds1820::r_humidity(char* buf, size_t, bool, int)
{
	auto lk = lock();
	std::sprintf(buf, "%d", hum);
	return std::strlen(buf);
}

int ds1820::r_cfg(char* buf, size_t size, bool uncached, int)
{
	auto lk = lock();
	if (uncached)
		// coverity[sleep] - bus mutex must be held
		if (cfg_read() == -1)
			return -EAGAIN;
	//   |CRC |  RES    |SW   1    2    3    4    5    6    7   | CFG  1    2    3    4    5    6    7  |FEA |OFF |MAJ |MIN |TYP |   OFF   |   FACT  |S   |IO  |TH  |TL  |TYP |THR |DIMD|DIMU|DIF |TM1 |TM2 |SWA0|SWA1|SWA2|SWA3|SWA4|SWA5|SWA6.
	for (int i = 0; i < DS1820_CFG_SIZE && (size_t)((i + 1) * 3) < size - 1; i++) {
		std::sprintf(buf + i * 3, "%02X ", cfg[i]);
	}
	return std::strlen(buf);
}

// Takes hex bytes without 0x separated by white space, e.g. "10 21 23",
// and writes them to the device starting at cfg[0] - the same layout
// r_cfg() prints, so its output can be written back as is
int ds1820::w_cfg(const char* buf, size_t size, int)
{
	auto lk = lock();
	uint8_t tmp[DS1820_CFG_SIZE];
	int len = 0;
	std::istringstream in(std::string(buf, size));
	std::string tok;

	while (in >> tok) {
		if (len == DS1820_CFG_SIZE || tok.size() > 2
				|| !std::all_of(tok.begin(), tok.end(), ::isxdigit))
			return -EINVAL;
		tmp[len++] = (uint8_t)std::stoul(tok, nullptr, 16);
	}
	if (len == 0)
		return -EINVAL;

	// coverity[sleep] - bus mutex must be held
	if (cfg_write(tmp, len) != len)
		return -EAGAIN;
	return size;
}

int ds1820::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	auto lk = lock();
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_read(path, buf, size, uncached);
}

// without this the base class would take the write and ignore it, the
// table's write handlers (w_cfg) would never be reached
int ds1820::fs_write(string& path, const char* buf, size_t size)
{
	auto lk = lock();
	int r = fs_table::write(*this, table, n_table, path, buf, size);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_write(path, buf, size);
}

int ds1820::cfg_read()
{
	auto lk = lock();
	int len = DS1820_CFG_SIZE;

#ifdef USE_I2C
	int i;
	uint8_t tmp[DS1820_CFG_SIZE];

	// the bus lock only for the transfer, cfg is the device's data and
	// updated below under the device lock alone
	{
		std::lock_guard<std::mutex> lock(ds->mtx);
		// coverity[sleep]
		if (!ds->selectChannel(bus))
			return -1;
		ds->reset();
		// coverity[sleep]
		ds->select(addr);
		// coverity[sleep]
		ds->write (0x85);

		for (i = 0; i < DS1820_CFG_SIZE - 1; i++)
			// coverity[sleep]
			tmp[i] = ds->read ();
	}
	std::memcpy(cfg, tmp, DS1820_CFG_SIZE - 1);
#endif
	return len;
}

// Writes len bytes of data to the device config, starting at cfg[0].
// Only once that succeeded they are also taken over into this->cfg, so
// the cached copy always matches the device.
int ds1820::cfg_write(const uint8_t* data, int len)
{
	auto lk = lock();
	int i;

	if (len > DS1820_CFG_SIZE)
		len = DS1820_CFG_SIZE;

	// bus mutex must be held for the whole 1-Wire
	// transaction: the transfer needs exclusive access to the shared
	// bus for its full duration, so releasing the lock mid-transfer
	// isn't an option here. Only for that: cfg is updated below under
	// the device lock alone.
	{
		std::lock_guard<std::mutex> lock(ds->mtx);

		// coverity[sleep] - bus mutex must be held
		if (!ds->selectChannel(bus))
			return -1;
		ds->reset();
		// coverity[sleep] - bus mutex must be held
		ds->select(addr);
		ds->write (0x86);

		for (i = 0; i < len - 1; i++)
			ds->write(data[i]);
	}
	// data may be this->cfg itself, just written back
	if (data != cfg)
		std::memcpy(cfg, data, len);
	return len;
}

/**
 * Returns temperature raw. celsius = tempRead * 16
 * mode 0: start conversion
 * 1: set alarms and reset
 * 2: read temperature and scratchpad
 * No retry handling because the data might not so important as next cycle
 * will come
*/
float ds1820::temp_read(const uint8_t flag)
{
	auto lk = lock();
	bool ret;

	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction
	std::lock_guard<std::mutex> m(ds->mtx);
	ret = ds->selectChannel(bus);
	if (!ret) {
		return -1;
	}
	ds->reset();
	// coverity[sleep] - bus mutex must be held
	ds->select(addr);
	switch (flag) {
	case 0:
		ds->write(STARTCONVO);
		return 0;
	case 2:
		ds->write(WRITESCRATCH);
		ds->write(35); // high alarm temp
		ds->write(10); // low alarm temp
		return 0;
	case 1:
	default:
		/* read temp */
		break;
	}
	ds->write(READSCRATCH);
	// Read all registers in a simple loop
	// byte 0: temperature LSB
	// byte 1: temperature MSB
	// byte 2: high alarm temp
	// byte 3: low alarm temp
	// byte 4: DS18S20: store for crc
	//         DS18B20 & DS1822: configuration register
	// byte 5: internal use & crc
	// byte 6: DS18S20: COUNT_REMAIN
	//         DS18B20 & DS1822: store for crc
	// byte 7: DS18S20: COUNT_PER_C
	//         DS18B20 & DS1822: store for crc
	// byte 8: SCRATCHPAD_CRC
	for (uint8_t i = 0; i < 9; i++)
		scratchPad[i] = ds->read();
	if (scratchPad[1] == 0xff)
		return -2;
#ifndef USE_I2C
	// dummy data for testing
	scratchPad[0] = 0x50; // LSB
	scratchPad[1] = 0x05; // MSB -> 85.00 °C
	scratchPad[5] = 60; // humidity
#endif // USE_I2C
	int16_t raw = (scratchPad[1] << 8) | scratchPad[0];
#if 0
#define cfg  (scratchPad[4] & 0x60)
	// at lower res, the low bits are undefined, so let's zero them
	if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
	else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
	else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
#endif
	//// default is 12 bit resolution, 750 ms conversion time
	// to be done by caller if needed
	hum = scratchPad[5];
	temp = (float)raw / 16.0;

	return temp;
}

// Only ever called once poll_check() has just returned 1.
int ds1820::poll()
{
	auto lk = lock();
	float old = temp;
	temp_read(0);
	//logger.info ("#1 reading %s/%s %d ...\n", rom.c_str(), path.c_str(), ret);
	temp_read(1);
	if (temp != old)
		logger.info (std::format("{} temp={} °, hum={} %", rom, temp, hum));
	return 1;
}
