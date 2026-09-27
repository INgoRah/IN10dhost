#include <fstream>
#include <iostream>
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
	{ "latched.*", 2, 8, false, nullptr, nullptr, &ds2408::r_latched, &ds2408::w_latched },
	{ "cfg", 3 * CFG_SIZE, 0, false, nullptr, nullptr, &ds2408::r_cfg, nullptr },
	{ "pin.*/name", 20, 8, false, nullptr, nullptr, &ds2408::r_pin_name, &ds2408::w_pin_name },
	{ "pin.*/func", 20, 8, false, nullptr, nullptr, &ds2408::r_pin_func, &ds2408::w_pin_func },
};
const size_t ds2408::n_table = sizeof(ds2408::table) / sizeof(ds2408::table[0]);

json ds2408::to_json() const {
	json j = OwDev::to_json(); // Get base class fields
	j["cfg"] = cfg; // Add ds2408 specific field

	return j;
};

void ds2408::from_json(const json& j) {
	OwDev::from_json(j); // Delegate common fields to base
	if (j.contains("cfg")) {
		j.at("cfg").get_to(cfg);
	}
}

// --- fs_table.h handlers -----------------------------------------------

int ds2408::r_byte(char* buf, size_t, bool uncached, int)
{
	if (uncached)
		reg_read(false);
	std::sprintf(buf, "%d", data[PIO_OUT]);
	return std::strlen(buf);
}

int ds2408::w_byte(const char* buf, size_t size, int)
{
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
	if (cfg[CFG_PIN_ID + idx] == CFG_OUT_PWM)
		std::sprintf(buf, "%d", level);
	else
		std::sprintf(buf, "%d", (data[PIO_OUT] & (0x1 << idx)) ? 1 : 0);
	return std::strlen(buf);
}

int ds2408::w_pio(const char* buf, size_t size, int idx)
{
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		logger.verbose(std::format("set PIO.{} = {}", idx, tmp));
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
	/* sensed from register 0x88 */
	std::sprintf(buf, "%d", (data[PIO_LS] & (0x1 << idx)) ? 1 : 0);
	return std::strlen(buf);
}

int ds2408::r_latched(char* buf, size_t, bool, int idx)
{
	/* read activity latch from register 0x8A */
	std::sprintf(buf, "%d", (data[PIO_LATCH] & (0x1 << idx)) ? 1 : 0);
	return std::strlen(buf);
}

int ds2408::w_latched(const char*, size_t size, int)
{
	// writing anything here clears the activity latches
	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction; see latch_reset()
	std::lock_guard<std::mutex> lock(ds->mtx);
	latch_reset();
	data[PIO_LATCH] = 0;
	return size;
}

int ds2408::r_cfg(char* buf, size_t size, bool uncached, int)
{
	if (uncached)
		if (cfg_read() == -1)
			return -EAGAIN;
	//   |CRC |  RES    |SW   1    2    3    4    5    6    7   | CFG  1    2    3    4    5    6    7  |FEA |OFF |MAJ |MIN |TYP |   OFF   |   FACT  |S   |IO  |TH  |TL  |TYP |THR |DIMD|DIMU|DIF |TM1 |TM2 |SWA0|SWA1|SWA2|SWA3|SWA4|SWA5|SWA6.
	for (int i = 0; i < CFG_SIZE && (size_t)((i + 1) * 3) < size - 1; i++) {
		std::sprintf(buf + i * 3, "%02X ", cfg[i]);
	}
	return std::strlen(buf);
}

int ds2408::r_pin_name(char* buf, size_t, bool, int idx)
{
	std::sprintf(buf, "n.%d", idx);
	return std::strlen(buf);
}

int ds2408::w_pin_name(const char*, size_t size, int)
{
	// per-pin names are not implemented yet; accept and discard rather
	// than falling through to the base class, whose fs_write() matches
	// "name" as a substring and would silently rename the device itself
	return size;
}

int ds2408::r_pin_func(char* buf, size_t, bool, int idx)
{
	std::sprintf(buf, "%X", cfg[CFG_PIN_ID + idx]);
	return std::strlen(buf);
}

int ds2408::w_pin_func(const char* buf, size_t size, int idx)
{
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

bool ds2408::vis_pio(int idx) const
{
	return (cfg[CFG_PIN_ID + idx] & CFG_OUT_MASK) != 0;
}

bool ds2408::vis_sensed(int idx) const
{
	return (cfg[CFG_PIN_ID + idx] & CFG_BTN_MASK) != 0;
}

// --- IFs, all table-driven ----------------------------------------------

std::vector<std::string> ds2408::fs_dir(string& path) const
{
	if (fs_table::in_instance_dir(table, n_table, path))
		return fs_table::dir(*this, table, n_table, path);

	std::vector<std::string> dir = OwDev::fs_dir(path);
	std::vector<std::string> extra = fs_table::dir(*this, table, n_table, path);
	dir.insert(dir.end(), extra.begin(), extra.end());
	return dir;
}

int ds2408::fs_attr(std::string& path) const
{
	int r = fs_table::attr(*this, table, n_table, path);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_attr(path);
}

int ds2408::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	logger.verbose("DS2408 read " + path + " " + rom);
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	if (r != fs_table::NOT_FOUND) {
		logger.log(LogLevel::DEBUG, "reading " + path + " -> " + std::string(buf) + "...");
		return r;
	}
	return OwDev::fs_read(path, buf, size, uncached);
}

int ds2408::fs_write(string& path, const char* buf, size_t size)
{
	int r = fs_table::write(*this, table, n_table, path, buf, size);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_write(path, buf, size);
}

void ds2408::begin(bool soft)
{
	if (soft)
		return;
	// if not soft read regs, cfg ...
	reg_read(true);
	// level_set
}

uint8_t ds2408::latch_reset()
{
	uint8_t retry, tmp;
	bool res;
#ifdef USE_I2C
	uint8_t err = 0;
#endif
	retry = LATCH_RESET_RETRY - 1;
	do {
		tmp = 0xff;
		res = ds->reset();
		if (res && ds->last_err == 0)
			ds->select(addr);
		if (ds->last_err == 0)
			ds->write (0xC3);
		if (ds->last_err == 0)
			tmp = ds->read();
#ifndef USE_I2C
		return 0xaa;
#else
		if (tmp == 0xAA)
			break;
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
	logger.log(LogLevel::DEBUG, "PIO cfg=" + std::to_string(cfg[CFG_PIN_ID + pio]));
	// check whether this is a level or simple IO
	if (cfg[CFG_PIN_ID + pio] == CFG_OUT_PWM) {
		// TODO Toggle leads to dim stages
		if (level_set(pio, lvl) != 0xAA)
			return -1;
		// store current level in data for readback until the next write
		level = lvl;
	} else {
		uint8_t tmp = 0;

		// TODO guard data struct
		if (state == TOGGLE) {
			if (data[PIO_OUT] & (0x1 << pio))
				tmp = data[PIO_OUT] & ~(0x01 << pio);
			else
				tmp = data[PIO_OUT] | (0x01 << pio);
		} else {
			if (state == ON)
				tmp = data[PIO_OUT] | (0x01 << pio);
			if (state == OFF)
				tmp = data[PIO_OUT] & ~(0x01 << pio);
		}
		pio_set(tmp);
	}
	return 0;
}

uint8_t ds2408::pio_set(uint8_t pio)
{
	uint8_t r, retry, err = 0;
	bool ret;

	logger.log(LogLevel::DEBUG, "set PIO " + std::to_string(pio));

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
				data[PIO_OUT] = pio;
				break;
			}
			if (err == 0)
				err = ds->last_err;
			usleep(1000);
		} while (--retry > 0);
		// if err && retry > 0: err = 0
		if (r == 0xAA) {
			// success
			latch_reset();
		}
	}
	json data = {
		{"bus", bus},
		{"rom", rom.c_str()},
		{"pio", pio}
	};
	plugins.action(ACT_DEV_CHANGE, 0, &data);

	return r;
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
#endif
		/* check for valid status register */
		data[STAT] = 0x00;
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
	// clear the alarm status
	tmp = this->latch_reset();

	logger.verbose(rom + " " + std::format(" read_regs OUT={:#x} LS={:#x} LATCH={:#x} STAT={:#x}", data[PIO_OUT], data[PIO_LS], data[PIO_LATCH], data[STAT]));
	return tmp;
}

int ds2408::cfg_read()
{
	int len = CFG_SIZE;

#ifdef USE_I2C
	int i;

	std::lock_guard<std::mutex> lock(ds->mtx);
	// coverity[sleep]
	if (!ds->selectChannel(bus))
		return -1;
	ds->reset();
	// coverity[sleep]
	ds->select(addr);
	// coverity[sleep]
	ds->write (0x85);

	for (i = 0; i < CFG_SIZE - 1; i++)
		// coverity[sleep]
		cfg[i] = ds->read ();
#endif
	return len;
}

int ds2408::cfg_write(int len)
{
	int i;

	if (len > CFG_SIZE)
		len = CFG_SIZE;

	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction: the transfer needs exclusive access to the shared
	// bus for its full duration, so releasing the lock mid-transfer
	// isn't an option here
	std::lock_guard<std::mutex> lock(ds->mtx);

	if (!ds->selectChannel(bus))
		return -1;
	ds->reset();
	ds->select(addr);
	ds->write (0x86);

	for (i = 0; i < len - 1; i++)
		ds->write(cfg[i]);

	return len;
}

/*
	cmd:
	- TMR_TYPE_BRIGHTNESS: set current brightness: level_set(0, 0, 0xE3, light)
	- TMR_TYPE_THRESHOLD
*/
uint8_t ds2408::level_set(uint8_t pio, uint8_t level, uint8_t cmd, uint8_t val)
{
#ifdef USE_I2C
	uint8_t data[5] = { 0xC5, cmd, pio, val, level };
	uint16_t crc;
#endif
	logger.log(LogLevel::DEBUG, "set PIO level=" + std::to_string(level) + " for PIO " + std::to_string(pio));

#ifndef USE_I2C
	(void)cmd;
	(void)val;
	return 0xaa;
#else
	/* if setting any level, a level 0 means stop */
	if (level == 0 && cmd == 0xDD)
		/* dim down */
		data[1] = TMR_TYPE_STOP_DIM;
	// coverity[sleep] - bus mutex must be held for the whole 1-Wire
	// transaction
	std::lock_guard<std::mutex> lock(ds->mtx);
	if (!ds->selectChannel(bus))
		return 0xff;
	if (!ds->reset())
		return 0xff;

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
	uint16_t crc16 = ds->crc16(data, 5, 0);
	if (crc == static_cast<uint16_t>(~crc16))
		return 0xAA;


	return 0xff;
#endif
}
