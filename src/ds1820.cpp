#include <cmath>
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

	if (flag == 1)
		temp_ok = false;

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
#ifdef TESTING
	if (sim_crc_fail > 0) {
		sim_crc_fail--;
		scratchPad[8] ^= 0x5a;
	}
#endif
	// a bit error (1024.12 degrees seen): the reading is dropped
	if (crc8(scratchPad, 8) != scratchPad[8]) {
		logger.warn(std::format("{} scratchpad CRC wrong: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
			rom, scratchPad[0], scratchPad[1], scratchPad[2], scratchPad[3], scratchPad[4],
			scratchPad[5], scratchPad[6], scratchPad[7], scratchPad[8]));
		return -3;
	}
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
	temp_ok = true;

	return temp;
}

// Only ever called once poll_check() has just returned 1.
// Two steps, the bus is never held waiting for the conversion: the
// poll due by the interval starts it, the next one DS1820_CONV_MS
// later reads it (see poll_ms()). Intervals too short for that convert
// and read at once, the reading may then still be the one before.
int ds1820::poll()
{
	auto lk = lock();

	if (interval_ms() < 2 * DS1820_CONV_MS) {
		converting = false;
		temp_read(0);
	} else if (!converting) {
		// failed: tried again by the interval
		converting = temp_read(0) == 0;
		return 1;
	} else {
		converting = false;
	}
	reading();
	// failed: the next cycle soon, see poll_ms()
	if (temp_ok || retries >= DS1820_RETRIES)
		retries = 0;
	else
		retries++;
	return 1;
}

void ds1820::reading()
{
	auto lk = lock();
	float old = temp;

	temp_read(1);
	if (temp_ok)
		poll_adapt(temp);
	if (temp != old)
		logger.info (std::format("{} temp={} °, hum={} %", rom, temp, hum));
}

// An alarm: TH/TL, on an owSlave also each change of the value (its
// config_info2[4] bit 0). Only the last conversion is read, none is
// started: one started here changes the value again, raises the next
// alarm, and the device is polled by its alarms instead of its
// interval. The two steps of poll() go on as they are.
int ds1820::alarm_poll()
{
	reading();
	return 1;
}

// Takes a new reading t: more than fast_delta off the one before
// switches to fast polling, see DS1820_FAST_DELTA. While fast, the
// readings count as settled as long as they stay within fast_delta of
// the first one counted.
void ds1820::poll_adapt(float t)
{
	auto lk = lock();
	bool on = fast_delta > 0 && fast_factor > 1;

	if (!on) {
		fast = false;
	} else if (!fast) {
		if (last_temp && std::fabs(t - *last_temp) > fast_delta) {
			fast = true;
			settle_ref = t;
			settled = 0;
			logger.info(std::format("{}: {:.2f} -> {:.2f} °C, polling every {:.1f} s",
				display_name(), *last_temp, t, interval_ms() / 1000.0));
		}
	} else if (std::fabs(t - settle_ref) <= fast_delta) {
		if (++settled >= DS1820_FAST_SETTLED) {
			fast = false;
			logger.info(std::format("{}: settled at {:.2f} °C, polling every {} s",
				display_name(), t, poll_interval));
		}
	} else {
		settle_ref = t;
		settled = 0;
	}
	last_temp = t;
}

// 1 s at least, the resolution of the poll loop
long ds1820::interval_ms() const
{
	auto lk = lock();
	long ms = OwDev::poll_ms();

	if (fast && fast_factor > 1)
		ms = std::max(ms / fast_factor, 1000L);
	return ms;
}

// The conversion is read DS1820_CONV_MS after it was started, the next
// one starts that much earlier: one reading per interval_ms(). After a
// failed reading the next cycle DS1820_RETRY_MS later.
long ds1820::poll_ms() const
{
	auto lk = lock();
	long ms = interval_ms();

	if (converting)
		return DS1820_CONV_MS;
	if (retries > 0)
		return std::min(ms, DS1820_RETRY_MS);
	if (ms < 2 * DS1820_CONV_MS)
		return ms;
	return ms - DS1820_CONV_MS;
}

json ds1820::to_json() const
{
	auto lk = lock();
	json j = OwDev::to_json();

	j["fast_delta"] = fast_delta;
	j["fast_factor"] = fast_factor;
	return j;
}

void ds1820::from_json(const json& j)
{
	auto lk = lock();

	OwDev::from_json(j);
	if (j.contains("fast_delta"))
		j.at("fast_delta").get_to(fast_delta);
	if (j.contains("fast_factor"))
		j.at("fast_factor").get_to(fast_factor);
}
