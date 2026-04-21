#include <fstream>
#include <iostream>
#include <string>
#include "main.h"
#include "ow_devices.h"
#include "ard_i2c.h"
#include "ds2408.h"

static struct filetype DS2408[] = {
	{ "BYTE", 3 },
	{ "PIO.*", 3 },
	{ "sensed.*", 2 },
	{ "latched.*", 2 },
	{ "cfg", 3 * CFG_SIZE },
	{ "pin.*", 0 },
};

json ds2408::to_json() const {
	json j = OwDev::to_json(); // Get base class fields
	j["cfg"] = cfg; // Add ds2408 specific field

	return j;
};

void ds2408::from_json(const json& j) {
	OwDev::from_json(j); // Delegate common fields to base
	if (j.contains("cfg"))
		j.at("cfg").get_to(cfg);
}

std::vector<std::string> ds2408::fs_dir(string& path) const
{
	// add standards
	for (int i = 0; i < 8; i++) {
		if (path.find("pin." + std::to_string(i)) != string::npos) {
			std::vector<std::string> dir;
			dir.push_back("name");
			dir.push_back("func");
			return dir;
		}
	}
	std::vector<std::string> dir = OwDev::fs_dir(path);
	for (const auto& s : DS2408) {
		string sname = s.name;
		size_t pos = sname.find(".*");
		if (pos != std::string::npos) {
			for (int i = 0; i < 8; i++) {
				if (sname.find("PIO") != string::npos) {
					if (cfg[CFG_PIN_ID + i] & 0x20) {
						sname.replace(pos + 1, 1, std::to_string(i));
						dir.push_back(sname.c_str());
					}
					continue;
				}
				if (sname.find("sensed") != string::npos) {
					if (cfg[CFG_PIN_ID + i] & 0x10) {
						sname.replace(pos + 1, 1, std::to_string(i));
						dir.push_back(sname.c_str());
					}
					continue;
				}
				sname.replace(pos + 1, 1, std::to_string(i));
				dir.push_back(sname.c_str());
			}
		} else {
			dir.push_back(s.name);
		}
	}
	return dir;
}

int ds2408::fs_attr(std::string& path) const
{
	for (const auto& s : DS2408) {
		string sname = s.name;
		size_t pos = sname.find(".*");
		if (pos != std::string::npos) {
			for (int i = 0; i < 8; i++) {
				sname.replace(pos + 1, 1, std::to_string(i));
				if (path.find("PIO." + std::to_string(i)) != string::npos)
					return 1;
				if (path.find("sensed." + std::to_string(i)) != string::npos)
					return 1;
				if (path.find("pin." + std::to_string(i) + "/name") != string::npos)
					return 20;
				if (path.find("pin." + std::to_string(i) + "/func") != string::npos)
					return 20;
				if (path.find("pin." + std::to_string(i)) != string::npos) {
					return 0;
				}
			}
		} else {
			if (path.find(s.name) != std::string::npos)
				return s.suglen;
		}
	}

	// add standards
	return OwDev::fs_attr(path);
}

int ds2408::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	logger.verbose("DS2408 read " + path + " " + rom);
	if (path.find("BYTE") != string::npos) {
		if (uncached)
			reg_read(false);
		std::sprintf(buf, "%d", data[PIO_LS]);
		goto out;
	}
	if (path.find("cfg") != string::npos) {
		if (uncached)
			cfg_read();
		//   |CRC |  RES    |SW   1    2    3    4    5    6    7   | CFG  1    2    3    4    5    6    7  |FEA |OFF |MAJ |MIN |TYP |   OFF   |   FACT  |S   |IO  |TH  |TL  |TYP |THR |DIMD|DIMU|DIF |TM1 |TM2 |SWA0|SWA1|SWA2|SWA3|SWA4|SWA5|SWA6.
		for (int i = 0; i < CFG_SIZE && (size_t)(i * 3) < size - 1; i++) {
			std::sprintf(buf + i * 3, "%02X ", cfg[i]);
		}
		goto out;
	}
	for (const auto& s : DS2408) {
		string sname = s.name;
		size_t pos = sname.find(".*");
		if (pos != std::string::npos) {
			for (int i = 0; i < 8; i++) {
				sname.replace(pos + 1, 1, std::to_string(i));
				if (path.find(sname) != std::string::npos) {
					if (path.find(std::format("pin.{}/func", i)) != string::npos) {
						std::sprintf(buf, "%X", cfg[CFG_PIN_ID + i]);
						goto out;
					}
					if (path.find(std::format("pin.{}/name", i)) != string::npos) {
						std::sprintf(buf, "n.%d", i);
						goto out;
					}
					// read at 1 << i;
					if (path.find("PIO") != string::npos) {
						if (cfg[CFG_PIN_ID + i] == CFG_OUT_PWM)
							std::sprintf(buf, "%d", level);
						else
							std::sprintf(buf, "%d", (data[PIO_OUT] & (0x1 << i)) ? 1 : 0);
						goto out;
					}
					if (path.find("sensed") != string::npos) {
						/* sensed from register 0x88 */
						std::sprintf(buf, "%d", (data[PIO_LS] & (0x1 << i)) ? 1 : 0);
						goto out;
					}
					if (path.find("latched") != string::npos) {
						/* read activity latch from register 0x8A */
						std::sprintf(buf, "%d", (data[PIO_LATCH] & (0x1 << i)) ? 1 : 0);
						goto out;
					}
				}
			}
		}
	}
	return OwDev::fs_read(path, buf, size, uncached);
out:
	logger.log(LogLevel::DEBUG, "reading " + path + " -> " + std::string(buf) + "...");
	return std::strlen(buf);
}

int ds2408::fs_write(string& path, const char* buf, size_t size)
{
	string s;

	if (path.find("BYTE") != string::npos) {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		pio_set(tmp);
		return size;
	}
	for (const auto& s : DS2408) {
		string sname = s.name;
		size_t pos = sname.find(".*");
		if (pos != std::string::npos) {
			for (int i = 0; i < 8; i++) {
				sname.replace(pos + 1, 1, std::to_string(i));
				if (path.find(sname) != std::string::npos) {
					if (path.find(std::format("pin.{}/func", i)) != string::npos) {
						uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
						cfg[CFG_PIN_ID + i] = tmp;
						return size;
					}
					if (path.find("PIO") != string::npos) {
						uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
						if (mode != 0x10)
							ard_set(i, tmp);
						else {
							logger.verbose(std::format("set PIO.{} = {}", i, tmp));
							pin_switch(i, (tmp == 0 ? OFF : ON), tmp);
						}
						return size;
					}
					if (path.find(sname) != string::npos) {
						std::lock_guard<std::mutex> lock(ow->mtx);
						// reset the latches
						latch_reset();
						data[PIO_LATCH] = 0;
					}
				}
			}
		}
	}

	// add standards
	return OwDev::fs_write(path, buf, size);
}

uint8_t ds2408::ard_set(uint8_t pio, uint8_t val)
{
#ifndef USE_I2C
	(void)pio;
	(void)val;
#else
	uint8_t buf[5];
	uint8_t ret;
	int to = 100;

	int fd = open("/dev/i2c-0", 0x2f);
	if (fd <= 0) {
		logger.warn("error opening Arduino");
		return 0xff;
	}
	buf[0] = 0xe1;
	buf[1] = 0xe1;
	std::lock_guard<std::mutex> lock(ow->mtx);
	i2c_write_data(fd, buf, 2);
	do {
		delay(1);
		ret = i2c_read(fd);
	} while (ret != 0 && to-- > 0);
	if (to == 0 && ret != 0) {
		close(fd);
		logger.error("Arduino write issue");
		return 0xff;
	}
	buf[0] = 0x3;
	buf[1] = bus;
	buf[2] = id;
	buf[3] = pio;
	if (val)
		buf[4] = 100;
	else
		buf[4] = 0;
	i2c_write_data(fd, buf, 5);
	close(fd);

	data[0] = pio;
	// set ack to pending which will be set by the event handler
#endif
	return 0xAA;
}

uint8_t ds2408::latch_reset()
{
	uint8_t retry, tmp, err = 0;
#ifdef USE_I2C
	bool res;
#endif

	retry = LATCH_RESET_RETRY - 1;
	do {
		tmp = 0xff;
#ifdef USE_I2C
		res = ow->reset();
		if (res && ow->last_err == 0)
			ow->select(addr);
		if (ow->last_err == 0)
			ow->write (0xC3);
		if (ow->last_err == 0)
			tmp = ow->read();
#else
		tmp = 0xaa;
#endif
		if (tmp == 0xAA)
			break;
		if (ow->last_err != 0) {
			err = ow->last_err;
		}
		if (err == 0)
			err = ow->last_err;
		delay(LATCH_RESET_RETRY - retry);
	} while (--retry > 0);
#ifdef DEBUG
	if ((err && retry == 0) || (err && debug > 0)) {
		Serial.print (F("latch reset err="));
		Serial.print (err);
		if (retry == 0)
			Serial.println(F(" ERR! "));
		else {
			Serial.print(F(" retry="));
			Serial.println(retry);
		}
	}
#endif
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
		uint8_t tmp;

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
#ifdef USE_I2C
	uint8_t r, retry, err = 0;
	bool ret;
#endif

	std::lock_guard<std::mutex> lock(ow->mtx);
	logger.log(LogLevel::DEBUG, "set PIO " + std::to_string(pio) + " in mode " + std::to_string(mode));
#ifdef USE_I2C
	retry = PIOSET_RETRY - 1;
	do {
		r = 0xff;
		ret = ow->selectChannel(bus);
		if (ret)
			ret = ow->reset();
		if (ret)
			ow->select(addr);
		if (ow->last_err == 0)
			ow->write (0x5A);
		if (ow->last_err == 0)
			ow->write (pio);
		if (ow->last_err == 0)
			ow->write (0xFF & ~(pio));
		//if (ow->last_err == 0)
		// lets try a pseudo read at least to avoid
		// a hung dev
		r = ow->read();
		if (r == 0xAA) {
			data[PIO_OUT] = pio;
			break;
		}
		if (err == 0)
			err = ow->last_err;
		usleep(1000);
	} while (--retry > 0);
	// if err && retry > 0: err = 0
	if (r == 0xAA) {
		// success
		latch_reset();
	}
	return r;
#else
	data[0] = pio;
	data[PIO_OUT] = pio;
	return 0xAA;
#endif
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
#ifdef USE_I2C
	bool ret;
	uint8_t buf[3];  // Put everything in the buffer so we can compute CRC easily.
	// read data registers
	buf[0] = 0xF0;	// Read PIO Registers
	buf[1] = 0x88;	// LSB address
	buf[2] = 0x00;	// MSB address
#endif
	do {
		std::lock_guard<std::mutex> lock(ow->mtx);
		//wdt_reset(); ??
		/* read latch */
#ifdef USE_I2C
		ret = ow->selectChannel(bus);
		if (ow->last_err == 0)
			ret = ow->reset();
		if (ret && ow->last_err == 0)
			ow->select(addr);
		if (ow->last_err == 0)
			ow->write (buf, 3, 0);
		// 3 cmd bytes, 6 data bytes, 2 0xFF, 2 CRC16
		// 1:
		// try this: alway read not running into a watchdog
		// on the slave
		// if (ow->last_err == 0)
		ow->read (data, 10);
		/* check for valid status register */
		if (data[STAT] != 0xff)
			break;
		tmp = 0x55;
		if (err == 0)
			err = ow->last_err;
#else
		tmp = 0xaa;
#endif
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

	logger.verbose(rom + " " + std::format(" read_regs {:#x} {:#x} {:#x}", data[PIO_OUT], data[PIO_LS], data[PIO_LATCH]));
	return tmp;
}

int ds2408::cfg_read()
{
	int len = MAX_CFG_SIZE;

#ifdef USE_I2C
	int i;

	ow->selectChannel(bus);
	ow->reset();
	ow->select(addr);
	ow->write (0x85);

	for (i = 0; i < CFG_SIZE - 1; i++)
		cfg[i] = ow->read ();
#endif
	return len;
}

uint8_t ds2408::level_set(uint8_t pio, uint8_t level, uint8_t cmd, uint8_t val)
{
	uint8_t data[5] = { 0xC5, cmd, pio, val, level };
	uint16_t crc;
	bool ret;

	logger.log(LogLevel::DEBUG, "set PIO level=" + std::to_string(level) + " for PIO " + std::to_string(pio) + " in mode " + std::to_string(mode));

	/* if setting any level, a level 0 means stop */
	if (level == 0 && cmd == 0xDD)
		/* dim down */
		data[1] = TMR_TYPE_STOP_DIM;
#ifndef USE_I2C
	return 0xaa;
#endif
	ret = ow->selectChannel(bus);
	if (ret)
		ret = ow->reset();
	if (ret)
		ow->select(addr);
	for (int i = 0; i < 5; i++) {
		ow->write(data[i]);
		if (ow->last_err != 0)
			break;
	}
	//if (ow->last_err == 0)
	// lets try a pseudo read at least to avoid
	// a hung dev
	crc = ow->read();
	crc |= ow->read() << 8;
	uint16_t crc16 = ow->crc16(data, 5, 0);
	if (crc == static_cast<uint16_t>(~crc16))
		return 0xAA;


	return 0xff;
}
