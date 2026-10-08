#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "ard_i2c.h"
#include "ds2408.h"
#include "plugins.h"

#define LATCH_RESET_RETRY 20
/** Retries for activity latch reset */
#define ACTRES_RETRY 5;
/** Retries for register read */
#define REG_RETRY 20
#define PIOSET_RETRY 20

extern Plugins plugins;

// out-of-line definition of the private static members declared in
// ds2408.h; this counts as class scope for access control, so it can
// take the address of the private handlers below directly
const FsEntry<ds2408> ds2408::table[] = {
	{ "BYTE", 3, 0, false, nullptr, nullptr, &ds2408::r_byte, &ds2408::w_byte },
	{ "PIO.*", 3, 8, false, &ds2408::vis_pio, nullptr, &ds2408::r_pio, &ds2408::w_pio },
	{ "sensed.*", 2, 8, false, &ds2408::vis_sensed, nullptr, &ds2408::r_sensed, nullptr },
	{ "latched.*", 2, 8, false, &ds2408::vis_latched, nullptr, &ds2408::r_latched, &ds2408::w_latched },
	{ "cfg", 3 * DS2408_CFG_SIZE, 0, false, nullptr, nullptr, &ds2408::r_cfg, &ds2408::w_cfg },
	{ "pin.*/name", PIN_NAME_MAX, 8, false, nullptr, nullptr, &ds2408::r_pin_name, &ds2408::w_pin_name },
	{ "pin.*/func", 20, 8, false, nullptr, nullptr, &ds2408::r_pin_func, &ds2408::w_pin_func },
	{ "threshold", 3, 0, false, nullptr, nullptr, &ds2408::r_threshold, &ds2408::w_threshold },
	{ "brightness", 3, 0, false, nullptr, nullptr, &ds2408::r_brightness, &ds2408::w_brightness },
	{ "level.*", 3, 8, false, &ds2408::vis_level, nullptr, &ds2408::r_level, &ds2408::w_level },
};
const size_t ds2408::n_table = sizeof(ds2408::table) / sizeof(ds2408::table[0]);

json ds2408::to_json() const {
	auto lk = lock();
	json j = OwDev::to_json(); // Get base class fields
	// Add ds2408 specific fields
	j["cfg"] = cfg;
	j["threshold"] = threshold;
	j["brightness"] = brightness;
	j["pin_names"] = pin_name;

	return j;
};

void ds2408::from_json(const json& j) {
	auto lk = lock();
	OwDev::from_json(j); // Delegate common fields to base
	if (j.contains("cfg")) {
		j.at("cfg").get_to(cfg);
	}
	if (j.contains("threshold")) {
		j.at("threshold").get_to(threshold);
	}
	if (j.contains("brightness")) {
		j.at("brightness").get_to(brightness);
	}
	if (j.contains("pin_names")) {
		// tolerate a shorter list, the remaining pins keep their default
		const json& names = j.at("pin_names");
		for (size_t i = 0; i < names.size() && i < pin_name.size(); i++)
			names.at(i).get_to(pin_name[i]);
	}
}

// --- fs_table.h handlers -----------------------------------------------
// The fs_table.h handlers below are only called from fs_read()/
// fs_write()/fs_dir(), which already hold the device lock. They take it
// again (it is recursive) because static analysis cannot follow the call
// through the table's member pointers and would see unguarded access.

int ds2408::r_byte(char* buf, size_t, bool uncached, int)
{
	auto lk = lock();
	if (uncached)
		reg_read(false);
	std::sprintf(buf, "%d", data[PIO_OUT]);
	return std::strlen(buf);
}

int ds2408::w_byte(const char* buf, size_t size, int)
{
	auto lk = lock();
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		pio_set(tmp);
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int ds2408::r_pio(char* buf, size_t, bool, int idx)
{
	auto lk = lock();
	if (cfg[CFG_PIN_ID + idx] == CFG_OUT_PWM)
		std::sprintf(buf, "%d", level_pct[idx]);
	else
		// inverted: 1 = OFF (output latch bit 0), 0 = ON (latch bit 1)
		std::sprintf(buf, "%d", (data[PIO_OUT] & (0x1 << idx)) ? 0 : 1);
	return std::strlen(buf);
}

int ds2408::w_pio(const char* buf, size_t size, int idx)
{
	auto lk = lock();
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		logger.verbose(std::format("set PIO.{} = {}", idx, tmp));
		// inverted for plain IO: writing 1 clears the output latch bit,
		// writing 0 sets it (see pin_switch); PWM takes the level as is
		pin_switch(idx, (tmp == 0 ? OFF : ON), tmp);
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int ds2408::r_sensed(char* buf, size_t, bool, int idx)
{
	auto lk = lock();
	/* sensed from register 0x88 */
	std::sprintf(buf, "%d", (data[PIO_LS] & (0x1 << idx)) ? 1 : 0);
	return std::strlen(buf);
}

int ds2408::r_latched(char* buf, size_t, bool, int idx)
{
	auto lk = lock();
	/* activity latch as collected by the alarm handling, alarm_read() */
	std::sprintf(buf, "%d", (latched & (0x1 << idx)) ? 1 : 0);
	return std::strlen(buf);
}

// Writing 0 to any latched.N clears all latches of the device and its
// alarm flag, which also drops it from /alarm. Nothing else clears them.
int ds2408::w_latched(const char* buf, size_t size, int)
{
	auto lk = lock();
	std::string s(buf, size);

	// a C string's NUL, or the newline of a shell redirect
	s = s.substr(0, s.find('\0'));
	while (!s.empty() && std::isspace((unsigned char)s.back()))
		s.pop_back();
	if (s != "0")
		return -EINVAL;
	if (data[PIO_LATCH] != 0) {
		logger.verbose(rom + " latch reset: cleared from the file system");
		// coverity[sleep] - bus mutex must be held for the whole 1-Wire
		// transaction; see latch_reset()
		std::lock_guard<std::mutex> lock(ds->mtx);
		latch_reset();
	}
	data[PIO_LATCH] = 0;
	latched = 0;
	alarm = false;
	return size;
}

int ds2408::r_cfg(char* buf, size_t size, bool uncached, int)
{
	auto lk = lock();
	if (uncached)
		// coverity[sleep] - bus mutex must be held
		if (cfg_read() == -1)
			return -EAGAIN;
	//   |CRC |  RES    |SW   1    2    3    4    5    6    7   | CFG  1    2    3    4    5    6    7  |FEA |OFF |MAJ |MIN |TYP |   OFF   |   FACT  |S   |IO  |TH  |TL  |TYP |THR |DIMD|DIMU|DIF |TM1 |TM2 |SWA0|SWA1|SWA2|SWA3|SWA4|SWA5|SWA6.
	for (int i = 0; i < DS2408_CFG_SIZE && (size_t)((i + 1) * 3) < size - 1; i++) {
		std::sprintf(buf + i * 3, "%02X ", cfg[i]);
	}
	return std::strlen(buf);
}

// Takes hex bytes without 0x separated by white space, e.g. "10 21 23",
// and writes them to the device starting at cfg[0] - the same layout
// r_cfg() prints, so its output can be written back as is
int ds2408::w_cfg(const char* buf, size_t size, int)
{
	auto lk = lock();
	uint8_t tmp[DS2408_CFG_SIZE];
	int len = 0;
	std::istringstream in(std::string(buf, size));
	std::string tok;

	while (in >> tok) {
		if (len == DS2408_CFG_SIZE || tok.size() > 2
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

int ds2408::r_pin_name(char* buf, size_t size, bool, int idx)
{
	auto lk = lock();
	if (pin_name[idx].empty())
		std::snprintf(buf, size, "PIO.%d", idx);
	else
		std::snprintf(buf, size, "%s", pin_name[idx].c_str());
	return std::strlen(buf);
}

// Handled here rather than by the base class, whose fs_write() matches
// "name" as a substring and would rename the device itself. An empty
// name resets the pin to its default.
int ds2408::w_pin_name(const char* buf, size_t size, int idx)
{
	auto lk = lock();
	std::string s(buf, size);

	// trim the newline a shell redirect adds, or a C string's NUL
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0'))
		s.pop_back();
	if (s.size() > PIN_NAME_MAX)
		return -EINVAL;
	pin_name[idx] = std::move(s);
	return size;
}

int ds2408::r_pin_func(char* buf, size_t, bool, int idx)
{
	auto lk = lock();
	switch(cfg[CFG_PIN_ID + idx]) {
		case 0x21:
			std::sprintf(buf, "OUT");
			break;
		case 0x23:
			std::sprintf(buf, "PWM");
			break;
		case 0x10:
			std::sprintf(buf, "BTN");
			break;
		case 0x11:
			std::sprintf(buf, "SW");
			break;
		case 0x05:
			std::sprintf(buf, "PASS");
			break;
		case 0x06:
			std::sprintf(buf, "INV");
			break;
		case 0x07:
			std::sprintf(buf, "INV_PU");
			break;
		case 0x02:
			std::sprintf(buf, "ACT_HIGH");
			break;
		default:
			std::sprintf(buf, "%X", cfg[CFG_PIN_ID + idx]);
			break;
	}
	return std::strlen(buf);
}

int ds2408::w_pin_func(const char* buf, size_t size, int idx)
{
	auto lk = lock();
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		cfg[CFG_PIN_ID + idx] = tmp;
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int ds2408::r_threshold(char* buf, size_t, bool, int)
{
	auto lk = lock();
	std::sprintf(buf, "%d", threshold);
	return std::strlen(buf);
}

int ds2408::w_threshold(const char* buf, size_t size, int)
{
	auto lk = lock();
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		if (threshold_set(tmp) == 0)
			return size;
		return -EAGAIN;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int ds2408::r_brightness(char* buf, size_t, bool, int)
{
	auto lk = lock();
	std::sprintf(buf, "%d", brightness);
	return std::strlen(buf);
}

int ds2408::w_brightness(const char* buf, size_t size, int)
{
	auto lk = lock();
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		if (brightness_set(tmp) == 0)
			return size;
		return -EAGAIN;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

bool ds2408::vis_pio(int idx) const
{
	auto lk = lock();
	switch (cfg[CFG_PIN_ID + idx]) {
		case 0xff:
		case 0:
			return false;
		default:
			return (cfg[CFG_PIN_ID + idx] & CFG_OUT_MASK) != 0;
	}
}

bool ds2408::vis_level(int idx) const
{
	auto lk = lock();
	return cfg[CFG_PIN_ID + idx] == CFG_OUT_PWM;
}

// level.* is the PWM level in percent, 0 = off .. 100 = full on, handed
// to level_set() as is.
int ds2408::r_level(char* buf, size_t, bool, int idx)
{
	auto lk = lock();
	// readable only where it is listed, see vis_level()
	if (!vis_level(idx))
		return -ENOENT;
	std::sprintf(buf, "%d", level_pct[idx]);
	return std::strlen(buf);
}

int ds2408::w_level(const char* buf, size_t size, int idx)
{
	auto lk = lock();
	int pct;

	if (!vis_level(idx))
		return -ENOENT;
	try {
		pct = std::stoi(std::string(buf, size));
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
	if (pct < 0 || pct > 100)
		return -EINVAL;

	if (level_set((uint8_t)idx, (uint8_t)pct) != 0xAA)
		return -EAGAIN;

	return size;
}

bool ds2408::vis_sensed(int idx) const
{
	auto lk = lock();
	switch (cfg[CFG_PIN_ID + idx]) {
		case 0xff:
		case 0:
			return false;
		default:
			return (vis_pio(idx)) == 0;
	}
}

bool ds2408::vis_latched(int idx) const
{
	auto lk = lock();
	switch (cfg[CFG_PIN_ID + idx]) {
		case 0xff:
		case 0:
			return false;
		default:
			return true;
	}
}

// --- IFs, all table-driven ----------------------------------------------

std::vector<std::string> ds2408::fs_dir(string& path) const
{
	auto lk = lock();
	if (fs_table::in_instance_dir(table, n_table, path))
		return fs_table::dir(*this, table, n_table, path);

	std::vector<std::string> dir = OwDev::fs_dir(path);
	std::vector<std::string> extra = fs_table::dir(*this, table, n_table, path);
	dir.insert(dir.end(), extra.begin(), extra.end());
	return dir;
}

int ds2408::fs_attr(std::string& path) const
{
	auto lk = lock();
	int r = fs_table::attr(*this, table, n_table, path);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_attr(path);
}

int ds2408::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	auto lk = lock();
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	if (r != fs_table::NOT_FOUND) {
		return r;
	}
	return OwDev::fs_read(path, buf, size, uncached);
}

int ds2408::fs_write(string& path, const char* buf, size_t size)
{
	auto lk = lock();
	int r = fs_table::write(*this, table, n_table, path, buf, size);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_write(path, buf, size);
}

void ds2408::begin(bool soft)
{
	auto lk = lock();
	if (soft)
		return;
	// if not soft read regs, cfg ...
	//cfg_read();
	reg_read(true);
	// level_set
}

uint8_t ds2408::latch_reset()
{
	auto lk = lock();
	uint8_t retry, tmp;
	bool res;
#ifdef USE_I2C
	uint8_t err = 0;
#endif
	retry = LATCH_RESET_RETRY - 1;
	do {
		tmp = 0xff;
		// select this device's bus first: called on its own (w_latched)
		// the controller may still be on whichever bus was used last,
		// and the reset would go to a device on the wrong bus
		res = ds->selectChannel(bus) && ds->reset();
		if (res && ds->last_err == 0)
			// coverity[sleep] - bus mutex must be held
			ds->select(addr);
		if (ds->last_err == 0)
			ds->write (0xC3);
		if (ds->last_err == 0)
			tmp = ds->read();
#ifndef USE_I2C
		// simulated bus: behave like a successful reset
		data[PIO_LATCH] = 0;
		return 0xaa;
#else
		// data[PIO_LATCH] mirrors the device: cleared once its latch
		// is. Whoever needs the latches takes a copy first, see
		// reg_read() (last_latch) and alarm_read().
		if (tmp == 0xAA) {
			data[PIO_LATCH] = 0;
			break;
		}
		if (ds->last_err != 0) {
			err = ds->last_err;
		}
		if (err == 0)
			err = ds->last_err;
		delay(LATCH_RESET_RETRY - retry);
#endif
	} while (--retry > 0);

	if (retry == 0)
		return 0xff;

	return tmp;
}

uint8_t ds2408::pin_switch(uint8_t pio, enum _pio_mode state, uint8_t lvl)
{
	auto lk = lock();
	//logger.log(LogLevel::DEBUG, "PIO cfg=" + std::to_string(cfg[CFG_PIN_ID + pio]));
	// check whether this is a level or simple IO
	if (cfg[CFG_PIN_ID + pio] == CFG_OUT_PWM) {
		if (state == TOGGLE ) {
			if (level_pct[pio] == 0)
				lvl = 100;
			else
				lvl = 0;
		} else if (state == OFF) {
			lvl = 0;
		} else if (state == ON && lvl == 0) {
			// ON without a level, e.g. from the switch handler: full on
			lvl = 100;
		}
		// TODO Toggle leads to dim stages
		// level_set() keeps the level for readback once it is applied
		if (level_set(pio, lvl, TMR_TYPE_ON) != 0xAA)
			return -1;
	} else {
		uint8_t tmp = 0;

		// TODO guard data struct
		if (state == TOGGLE) {
			if (data[PIO_OUT] & (0x1 << pio))
				tmp = data[PIO_OUT] & ~(0x01 << pio);
			else
				tmp = data[PIO_OUT] | (0x01 << pio);
		} else {
			// active low: ON clears the latch bit, OFF sets it
			if (state == ON)
				tmp = data[PIO_OUT] & ~(0x01 << pio);
			if (state == OFF)
				tmp = data[PIO_OUT] | (0x01 << pio);
		}
		pio_set(tmp);
	}
	return 0;
}

uint8_t ds2408::pio_set(uint8_t pio)
{
	auto lk = lock();
	uint8_t r, retry, err = 0;
	bool ret;

	logger.debug(std::format("Set {} {} PIO {:#x}", rom, name, pio));

	{
	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction (reset/select/write/read + retries)
	std::lock_guard<std::mutex> lock(ds->mtx);
		retry = PIOSET_RETRY - 1;
		do {
			ret = ds->selectChannel(bus);
			if (ret)
				ret = ds->reset();
			if (ret)
				// coverity[sleep] - bus mutex must be held
				ds->select(addr);
			if (ds->last_err == 0)
				ds->write (0x5A);
			if (ds->last_err == 0)
				ds->write (pio);
			if (ds->last_err == 0)
				ds->write (0xFF & ~(pio));
			//if (ds->last_err == 0)
			// lets try a pseudo read at least to avoid
			// a hung dev
			r = ds->read();
#ifndef USE_I2C
			r = 0xAA;
#endif
			if (r == 0xAA) {
				break;
			}
			if (err == 0)
				err = ds->last_err;
			// coverity[sleep] - bus mutex must be held
			usleep(1000);
		} while (--retry > 0);
		// if err && retry > 0: err = 0
		if (r == 0xAA) {
			// success
			logger.verbose(rom + std::format(" latch reset: after PIO write {:#x}", pio));
			latch_reset();
		}
	}
	if (r == 0xAA && data[PIO_OUT] != pio) {
		data[PIO_OUT] = pio;
		// sent once the device is unlocked, see OwDev::notify_change()
		notify_change(json{
			{"bus", bus},
			{"type", type},
			{"rom", rom.c_str()},
			{"pio", pio}
		});
	}
	return r;
}

/* Register read for an alarm, called from the alarm handling. The read
 * also resets the latches on the device, so the ones it got are handed
 * back in latch for the caller to work on, and added to the ones kept
 * for latched.* (a second alarm before they were cleared does not lose
 * the first). The device is flagged for /alarm. Returns what
 * reg_read() returns. */
uint8_t ds2408::alarm_read(uint8_t& latch)
{
	auto lk = lock();
	uint8_t res = reg_read(true);

	latch = last_latch;
	if ((res == 0xaa || res == 0xff) && latch != 0xff)
		latched |= latch;
	alarm = true;
	return res;
}

/* Read DS2408 registers
 * [0] PIO Logic State
 * [1] Output latch
 * [2] Activity latch state
 * [3] Conditional search channel selection (unused)
 * [4] Conditional search polarity selection (unused)
 * [5] Status
 * [6] Status ext 1, used for press time detection
 * [7] Status ext 2
 *
 * Returns 0xaa in case of successful read and latch reset
 * Returns 0xff in case of latch reset issue
 * Returns 0 in case of bus error or timeout -> to be repeated
 * */
uint8_t ds2408::reg_read(bool latch_reset)
{
	auto lk = lock();
	uint8_t tmp, err = 0;
	uint8_t retry = REG_RETRY - 1;
	bool ret;
	uint8_t buf[3];  // Put everything in the buffer so we can compute CRC easily.
	// read data registers
	buf[0] = 0xF0;	// Read PIO Registers
	buf[1] = 0x88;	// LSB address
	buf[2] = 0x00;	// MSB address
	do {
		//wdt_reset(); ??
		/* read latch */
		std::lock_guard<std::mutex> lock(ds->mtx);
		// coverity[sleep]
		ret = ds->selectChannel(bus);
		if (ds->last_err == 0)
			ret = ds->reset();
		if (ret && ds->last_err == 0)
			// coverity[sleep]
			ds->select(addr);
		if (ds->last_err == 0)
			// coverity[sleep]
			ds->write (buf, 3, 0);
		// 3 cmd bytes, 6 data bytes, 2 0xFF, 2 CRC16
		// 1:
		// try this: alway read not running into a watchdog
		// on the slave
		// if (ds->last_err == 0)
#ifdef USE_I2C
		// coverity[sleep]
		ds->read (data, 10);
#else
		uint8_t dummy[10];
		ds->read (dummy, 10);
		data[STAT] = 0x00;
#endif
		/* check for valid status register */
		if (data[STAT] != 0xff)
			break;
		if (err == 0)
			err = ds->last_err;
		// if we got here, there is an issue and we
		// will try again
		//delay(5);
	} while (--retry > 0);
	if (err) {
		if ((retry == 0 || data[STAT] == 0xff) && !latch_reset)
			return 0xff;
	}
	// clear the alarm status, only when asked: any other read (e.g. an
	// uncached BYTE) must not swallow latches an alarm has not read yet
	// copy before the reset below clears data[PIO_LATCH]
	last_latch = data[PIO_LATCH];
	if (latch_reset) {
		logger.verbose(rom + std::format(" latch reset: after register read, LATCH={:#x}", data[PIO_LATCH]));
		tmp = this->latch_reset();
	} else {
		tmp = 0xaa;
	}

	logger.verbose(rom + " " + std::format(" read_regs OUT={:#x} LS={:#x} LATCH={:#x} STAT={:#x}", data[PIO_OUT], data[PIO_LS], last_latch, data[STAT]));
	if (data[STAT] & 0x40) {
		/* status in 5 signals a dimming down */
		level_pct[0] = 0;
	}

	return tmp;
}

int ds2408::cfg_read()
{
	auto lk = lock();
	int len = DS2408_CFG_SIZE;

#ifdef USE_I2C
	int i;
	uint8_t tmp[DS2408_CFG_SIZE];

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

		for (i = 0; i < DS2408_CFG_SIZE - 1; i++)
			// coverity[sleep]
			tmp[i] = ds->read ();
	}
	std::memcpy(cfg, tmp, DS2408_CFG_SIZE - 1);
#endif
	return len;
}

// Writes len bytes of data to the device config, starting at cfg[0].
// Only once that succeeded they are also taken over into this->cfg, so
// the cached copy always matches the device.
// storing the data into EERPOM is done by the device itself, if
// len = DS2408_CFG_SIZE;
// data[21] = 0x55;
int ds2408::cfg_write(const uint8_t* data, int len)
{
	auto lk = lock();
	int i;

	if (len > DS2408_CFG_SIZE)
		len = DS2408_CFG_SIZE;

	// bus mutex must be held for the whole 1-Wire
	// transaction: the transfer needs exclusive access to the shared
	// bus for its full duration, so releasing the lock mid-transfer
	// isn't an option here
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

/*
	level: 0..100 percent, larger values are taken as 100. The device
	takes 0..254, so it is converted here, like the Arduino host did in
	SwitchHandler::setLevel() before calling ds2408xPinSet().
	The level a TMR_TYPE_ON sets is kept per pin in level_pct once the
	device confirmed it, for everyone calling this - level.*, PIO.* and
	the switch handler through pin_switch() - to read back.
	cmd:
	- TMR_TYPE_BRIGHTNESS: set current brightness: level_set(0, 0, 0xE3, light)
	- TMR_TYPE_THRESHOLD
*/
uint8_t ds2408::level_set(uint8_t pio, uint8_t level, uint8_t cmd, uint8_t val)
{
	auto lk = lock();
	uint8_t pct = level > 100 ? 100 : level;

	level = (uint8_t)(pct * 254 / 100);
#ifdef USE_I2C
	uint8_t data[5] = { 0xC5, cmd, pio, val, level };
	uint16_t crc;
#endif
	logger.log(LogLevel::DEBUG, "set PIO level=" + std::to_string(pct) + "% (" + std::to_string(level) + ") for PIO " + std::to_string(pio));

#ifndef USE_I2C
	(void)val;
	if (cmd == TMR_TYPE_ON)
		level_pct[pio] = pct;
	return 0xaa;
#else
	/* if setting any level, a level 0 means stop */
	if (cmd == TMR_TYPE_ON) {
		if (level == 0)
			/* dim down */
			data[1] = TMR_TYPE_STOP_DIM;
		data[3] = level;
	}
	logger.debug(std::format("send cmd {:#x} {:#x} {:#x} {:#x}", data[1], data[2], data[3], data[4]));
	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction
	{
	std::lock_guard<std::mutex> lock(ds->mtx);
	// coverity[sleep] - bus mutex must be held
	if (!ds->selectChannel(bus))
		return 0xff;
	// coverity[sleep] - bus mutex must be held
	if (!ds->reset())
		return 0xff;

	// coverity[sleep] - bus mutex must be held
	ds->select(addr);
	for (int i = 0; i < 5; i++) {
		ds->write(data[i]);
		if (ds->last_err != 0)
			break;
	}
	//if (ds->last_err == 0)
	// lets try a pseudo read at least to avoid
	// a hung dev
	crc = ds->read();
	crc |= ds->read() << 8;
	}
	// no bus access, so outside the bus lock above
	uint16_t crc16 = DS2482::crc16(data, 5, 0);
	if (crc == static_cast<uint16_t>(~crc16)) {
		logger.verbose(std::format("CRC ok {:#x}", crc));
		if (cmd == TMR_TYPE_ON)
			level_pct[pio] = pct;
		return 0xAA;
	}
	else
		logger.warn(std::format("CRC mismatch {:#x} <> {:#x}", crc, ~crc16));


	return 0xff;
#endif
}

int ds2408::brightness_set(uint8_t brightness)
{
	auto lk = lock();
	this->brightness = brightness;
	if (level_set(0, 0, TMR_TYPE_BRIGHTNESS, brightness) != 0xAA)
		return -EAGAIN;
	return 0;
}

int ds2408::threshold_set(uint8_t threshold)
{
	auto lk = lock();
	this->threshold = threshold;
	if (level_set(0, 0, TMR_TYPE_THRESHOLD, threshold) != 0xAA)
		return -EAGAIN;
	return 0;
}
