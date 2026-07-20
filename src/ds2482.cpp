// SPDX-License-Identifier: GPL-2.0-only
/*
  DS2482 library for Arduino
  Copyright (C) 2005  Ben Gardner <bgardner@wabtec.com>
  Copyright (C) 2009-2010 Paeae Technologies
  Copyright (C) 2020 INgo.Rah@gmx.net

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have readd a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

	crc code is from OneWire library

	-Updates:
		* fixed wireread busyWait (thanks Mike Jackson)
		* Modified search function (thanks Gary Fariss)
		* adapted the class/function layout to have a common function API with OneWire.h

  https://github.com/paeaetech/paeae.git
*/
#include "ds2482.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <cstring>
#include <cerrno>
#include <format>

// for data logger
#include <iostream>
#include <fstream> // Required for std::ofstream
#include <time.h>
#include <bitset>
#include "ring_buffer/ring_buffer.h"

#include "logger.h"

/* Values for DS2482_CMD_SET_READ_PTR */
#define DS2482_PTR_CODE_STATUS		0xF0
#define DS2482_PTR_CODE_DATA		0xE1
#define DS2482_PTR_CODE_CHANNEL		0xD2
#define DS2482_PTR_CODE_CONFIG		0xC3

#define DS2482_CMD_WRITE_CONFIG		0xD2
#define DS2482_CMD_1WIRE_RESET		0xB4

#define DS2482_CMD_SET_READPTR 0xe1
#define DS2482_CMD_CHANNEL 0xC3
#define DS2482_CMD_WRITE 0xa5
#define DS2482_CMD_READ 0x96
#define DS2482_CMD_1WIRE_TRIPLET	0x78

// State enum helpers for data logging
#define STATE_IDLE   '-'
#define STATE_RESET  '!'
#define STATE_SEARCH 'S'
#define STATE_READ   'R'
#define STATE_WRITE  'W'
#define STATE_CHANNEL 'C'

extern Logger logger;

typedef struct {
	uint64_t ts;
	uint8_t ch;
	uint8_t state;
	uint8_t data;
} log_data;
RingBuffer<log_data, 1024> data_log;


DS2482::DS2482(const std::string& i2c_dev, int address)
	: fd(-1), addr(address)
{
#ifdef USE_I2C
	fd = open(i2c_dev.c_str(), O_RDWR);
#else
	(void)i2c_dev;
	(void)address;
	fd = -1;
#endif
	ch = 0xff;
	_read_ptr = 0;
}

DS2482::~DS2482()
{
	if (fd >= 0)
		close(fd);
	data_log.clear();
}

uint64_t DS2482::get_now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}


// Helper to convert a byte to an 8-character binary string safely without std::bitset
void byte_to_binary_str(unsigned char byte, char* out_str) {
    for (int i = 7; i >= 0; --i) {
        out_str[7 - i] = (byte & (1 << i)) ? '1' : '0';
    }
    out_str[8] = '\0';
}

int DS2482::log_dump(char* buf, size_t size)
{
    char* current_ptr = buf;
    size_t remaining_size = size;
    int written = 0;

	if (!buf || size == 0)
		return 0;

	// Advanced lambda that accepts printf-style formatting arguments
	auto append_to_buf = [&](const char* format, auto... args) {
		if (remaining_size <= 1) return;

		// Safely format the dynamic string into the buffer
		int res = snprintf(current_ptr, remaining_size, format, args...);
		if (res > 0) {
			size_t actual_written = static_cast<size_t>(res);
			if (actual_written >= remaining_size) {
				actual_written = remaining_size - 1;
			}
			current_ptr += actual_written;
			remaining_size -= actual_written;
			written += actual_written;
		}
	};
 	// Appending VCD header blocks
	append_to_buf("$date\n  Today, 2026\n$end\n");
	append_to_buf("$timescale\n  1s\n$end\n");
	append_to_buf("$scope module 1wire $end\n");
	// Define variables
	append_to_buf("$var wire 8 a bus_state $end\n");
	append_to_buf("$var wire 8 b ch0_data $end\n");
	append_to_buf("$var wire 8 c ch1_data $end\n");
	append_to_buf("$var wire 8 d ch2_data $end\n");

	// Close scopes
	append_to_buf("$upscope $end\n");
	append_to_buf("$enddefinitions $end\n");
	int add = 0;
	uint64_t old_ts = 0;
	for (int i = 0; i < data_log.size();i++) {
		log_data l = data_log[i];
		char buf[32];
		if (old_ts >= l.ts) {
			add++;
			l.ts += add;
		} else {
			add = 0;
		}
		old_ts = l.ts;
		snprintf(buf, 32, "%lu", l.ts);
		append_to_buf("#%s\n", buf); // The timestamp
		byte_to_binary_str(l.state, (char *)buf);
		append_to_buf("b%s a\n", buf);
		byte_to_binary_str(l.data, (char *)buf);
		append_to_buf("b%s ", buf);
		switch (l.ch) {
			case 0:
				append_to_buf("b\n");
				break;
			case 1:
				append_to_buf("c\n");
				break;
			case 2:
				append_to_buf("d\n");
				break;
		}
	}
	return written;
}


void DS2482::log_event(uint8_t state, uint8_t data)
{
    uint64_t relative_us = get_now_us() - start_time;
	data_log.push(log_data{relative_us, ch, state, data});
}

// The 1-Wire CRC scheme is described in Maxim Application Note 27:
// "Understanding and Using Cyclic Redundancy Checks with Maxim iButton Products"
//
bool DS2482::check_crc16(const uint8_t* input, uint16_t len, const uint8_t* inverted_crc, uint16_t crc)
{
	crc = ~crc16(input, len, crc);
	return (crc & 0xFF) == inverted_crc[0] && (crc >> 8) == inverted_crc[1];
}

uint16_t DS2482::crc16(const uint8_t* input, uint16_t len, uint16_t crc)
{
	static const uint8_t oddparity[16] =
		{ 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };

	for (uint16_t i = 0 ; i < len ; i++) {
	  // Even though we're just copying a byte from the input,
	  // we'll be doing 16-bit computation with it.
	  uint16_t cdata = input[i];
	  cdata = (cdata ^ crc) & 0xff;
	  crc >>= 8;

	  if (oddparity[cdata & 0x0F] ^ oddparity[cdata >> 4])
		  crc ^= 0xC001;

	  cdata <<= 6;
	  crc ^= cdata;
	  cdata <<= 1;
	  crc ^= cdata;
	}
	return crc;
}

bool DS2482::init()
{
	last_err = 0;
#ifdef USE_I2C
	if (fd < 0) {
		printf("Failed to open I2C device %s\n", strerror(errno));
		return false;
	}

	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		printf("Failed to open I2C device\n");
		close(fd);
		return false;
	}
	if (ioctl(fd, I2C_TIMEOUT, 5) < 0) {
		printf("Failed to set I2C timeout\n");
		close(fd);
		return false;
	}
#endif
	start_time = get_now_us();

	return true;
}

void DS2482::set_error(int err_code, int def)
{
#ifdef USE_I2C
	switch (err_code) {
		case ETIMEDOUT:
			last_err = ERR_WIRE_TO;
			break;
		case EAGAIN:
		case EBUSY:
			last_err = ERR_WIRE_GEN;
			break;
		case ENXIO:
			last_err = ERR_WIRE_ADRNACK;
			break;
		default:
			last_err = def;
			break;
	}
#else
	(void)err_code;
	(void)def;
#endif
}

/* I2C write one byte */
void DS2482::_write(uint8_t b)
{
#ifdef USE_I2C
	int ret;
	struct i2c_msg msg = {
		.addr = addr,
		.flags = 0,
		.len = 1,
		.buf = &b
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret < 0) {
		_read_ptr = 0;
		set_error(ret, ERR_WRITE);
	}
	_read_ptr = DS2482_PTR_CODE_STATUS;
#else
	(void)b;
#endif
}

void DS2482::_write_cmd(uint8_t cmd, uint8_t data)
{
#ifdef USE_I2C
	uint8_t buf[2] = {cmd, data};

	struct i2c_msg msg = {
		.addr = addr,
		.flags = 0,
		.len = 2,
		.buf = buf
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	int ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret < 0) {
		_read_ptr = 0;
		set_error(ret, ERR_WRITE);
	}
#endif
	/* default all cmds leave in STATUS, except those */
	switch (cmd) {
		case DS2482_CMD_WRITE_CONFIG:
			_read_ptr = DS2482_PTR_CODE_CONFIG;
			break;
		case DS2482_CMD_CHANNEL:
			_read_ptr = DS2482_PTR_CODE_CHANNEL;
			break;
		case DS2482_CMD_SET_READPTR:
			_read_ptr = data;
			break;
		default:
			_read_ptr = DS2482_PTR_CODE_STATUS;
			break;
	}
}

void DS2482::resetDev()
{
#if 0
	ch = 0xff;
	_write(0xf0);
#endif
}

bool DS2482::configureDev(uint8_t config)
{
	(void)config;
	ch = 0xff;
#if 0
	busyWait();
	_write_cmd(0xd2, config | (~config)<<4);

	return _read() == config;
#endif
	return true;
}

/* This function is called only from this class
   Error handling (bus error from end) needs
   to be handled inside by checking the status
   member
*/
void DS2482::setReadPtr(uint8_t readPtr)
{
	if (_read_ptr == readPtr)
		return;

	_write_cmd (DS2482_CMD_SET_READPTR, readPtr);
}

/* i2c read one byte */
uint8_t DS2482::_read()
{
#ifdef USE_I2C
	uint8_t d;
	int ret;

	/* this can result in a timeout (ret == 0) */
	struct i2c_msg msgs[1] = {
		{
			.addr = 0,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = &d
		}
	};

	msgs[0].addr = addr;
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = msgs,
		.nmsgs = 1,
	};
	ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret < 0) {
		set_error(ret, ERR_READ);
		return 0xff;
	}
	return d;
#else
	// TODO simulate read with test data (array)
	return 0;
#endif
}


/* initializes the error code and set it to:
 * 0 .. success
 * 12 .. address send, NACK received
 * 13 .. data send, NACK received
 * 14 .. other twi error (lost bus arbitration, bus error, ..)
 * 15 .. timeout
 * 17 .. read error / timeout
 * 18 .. timeout waiting for ready
  */
uint8_t DS2482::busyWait()
{
	uint8_t res;
	int loopCount = 500;

	setReadPtr(DS2482_PTR_CODE_STATUS);
	res = _read();
	if (last_err != ERR_NONE) {
		/* can only be 1 .. 5
		if (last_err != 0 && last_err <= ERR_WIRE_TO) */
		last_err += ERR_BUSYWAIT;
		return DS2482_STATUS_INVAL;
	}
	while(res & DS2482_STATUS_BUSY) {
		usleep(10);
		res = _read();
		if (last_err == ERR_READ) {
			last_err = ERR_BUSYWAIT_RD;
			return DS2482_STATUS_INVAL;
		}
		if (--loopCount == 0 || last_err != ERR_NONE) {
			last_err = ERR_BUSYWAIT_TO;
			return DS2482_STATUS_INVAL;
		}
	}

	return res;
}

/* channel must be between 0 and 7
 * 32 .. address send, NACK received
 * 33 .. data send, NACK received
 * 34 .. other twi error (lost bus arbitration, bus error, ..)
 * 35 .. timeout
 * 37 .. read error / timeout
 * 38 .. timeout waiting for ready
 * 41..45 write error
 * 47 read error on cross check
 */
bool DS2482::selectChannel(uint8_t channel)
{
	uint8_t check;
	static const uint8_t chan_r[8] = { 0xB8, 0xB1, 0xAA, 0xA3, 0x9C, 0x95, 0x8E, 0x87 };
	static const uint8_t chan_w[8] = { 0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87 };
#if 0
	if (ch == channel)
		return true;
#endif
	last_err = ERR_NONE;
	if (busyWait() == DS2482_STATUS_INVAL) {
		/* err can be 11..18 */
		last_err += ERR_CHSEL1;
		goto sel_ch_error;
	}
	_write_cmd(DS2482_CMD_CHANNEL, chan_w[channel]);
	if (last_err != ERR_NONE) {
		/* err can be 1..5 : 41 ..45*/
		last_err += ERR_CHSEL2;
		goto sel_ch_error;
	}
	/* after channel selection the read pointer points
	to the channel register */

	check = _read();
#ifndef USE_I2C
	// simulate access if testing
	check = chan_r[channel];
#endif
	if (check != chan_r[channel]) {
		last_err = ERR_CHCHK;
		goto sel_ch_error;
	}

	ch = channel;
	return true;
sel_ch_error:
	ch = 0xff;
	logger.error(std::format("Failed to select channel {}, error code: {}", channel, last_err));
	return false;
}

/* 52 .. address send, NACK received (on busy wait)
 * 53 .. data send, NACK received
 * 54 .. other twi error (lost bus arbitration, bus error, ..)
 * 55 .. timeout
 * 57 .. read error / timeout
 * 58 .. timeout waiting for ready

 * 62 .. address send, NACK received (after first write)
 * 63 .. data send, NACK received
 * 64 .. other twi error (lost bus arbitration, bus error, ..)
 * 65 .. timeout

 * 66 .. address send, NACK received (after first write)
 * 67 .. data send, NACK received
 * 68 .. other twi error (lost bus arbitration, bus error, ..)
 * 69 .. timeout
 * 71 .. read error / timeout
 * 72 .. timeout waiting for ready
 */
bool DS2482::reset()
{
	last_err = ERR_NONE;
	busyWait();
	if (last_err != ERR_NONE) {
		/* err can be 11..18 */
		last_err += ERR_RESET1;
		return false;
	}
	_write(DS2482_CMD_1WIRE_RESET);
	if (last_err != ERR_NONE) {
		/* err can be 1..5 */
		last_err += ERR_RESET2;
		return false;
	}
	log_event(STATE_RESET, 0);
#ifdef USE_I2C
	usleep(400);
#else
	// simultate presence pulse if testing
	return true;
#endif
	uint8_t stat = busyWait();
	if (last_err != ERR_NONE) {
		/* err can be 11..18 */
		last_err += ERR_RESET3;
		return false;
	}
	return stat & DS2482_STATUS_PPD ? true : false;
}


/* 75 .. address send, NACK received (on busy wait)
 * 76 .. data send, NACK received
 * 77 .. other twi error (lost bus arbitration, bus error, ..)
 * 78 .. timeout
 * 80 .. read error / timeout
 * 81 .. timeout waiting for ready
 * 82 .. address send, NACK received (after write)
 * 83 .. data send, NACK received
 * 84 .. other twi error (lost bus arbitration, bus error, ..)
 * 85 .. timeout
 * */
uint8_t DS2482::write(uint8_t b, uint8_t power)
{
	(void)power;
	last_err = ERR_NONE;
	busyWait();
	if (last_err != ERR_NONE) {
		/* err can be 11..18 */
		last_err += ERR_WRITE1;
		return 0xff;
	}
	_write_cmd (DS2482_CMD_WRITE, b);
	if (last_err != ERR_NONE) {
		/* err can be 1..5 */
		last_err += ERR_WRITE2;
		return 0xff;
	}
	log_event(STATE_WRITE, b);

	return b;
}

uint8_t DS2482::read()
{
	uint8_t b;

	last_err = ERR_NONE;
	busyWait();
	if (last_err != ERR_NONE) {
		/* err can be 11..18 */
		last_err += ERR_READ1;
		return 0xff;
	}
	_write(DS2482_CMD_READ);
	if (last_err != ERR_NONE) {
		last_err += ERR_READ2;
		return 0xff;
	}
	busyWait();
	if (last_err != ERR_NONE) {
		last_err += ERR_READ3;
		return 0xff;
	}
	setReadPtr(DS2482_PTR_CODE_DATA);
	b = _read();
	log_event(STATE_READ, b);

	return b;
}

void DS2482::select(const uint8_t rom[8])
{
	write(OW_MATCH_ROM);
	if (last_err != ERR_NONE) {
		last_err += ERR_SELECT1;
		return;
	}
	for (int i = 0; i < 8; i++) {
		write(rom[i]);
		if (last_err != ERR_NONE) {
			last_err += ERR_SELECT2;
			return;
		}
	}
}

void DS2482::target_search(uint8_t family_code)
{
	// Reset the search state
	reset_search();

	// Set the family code in the address buffer
	searchAddress[0] = family_code;

	// Force the search to follow these 8 bits without looking for discrepancies
	// 64 is a marker often used to indicate "pre-filled" or "limit"
	searchLastDisrepancy = 64;
	searchLastFamilyDiscrepancy = 0;
}

void DS2482::reset_search()
{
	searchExhausted = 0;
	searchLastDisrepancy = 0;
	last_err = ERR_NONE;

	for(uint8_t i = 0; i<8; i++)
		searchAddress[i] = 0;
}

bool DS2482::search(uint8_t *newAddr, bool search_mode)
{
	uint8_t i;
	uint8_t direction;
	uint8_t last_zero = 0;
	uint8_t stat;

	if (searchExhausted)
		return false;

	reset();
	write(search_mode ? OW_SEARCH_ROM : OW_COND_SEARC_ROM);

	if (last_err != ERR_NONE) {
		last_err += ERR_SRCH2;
		return false;
	}
#ifndef USE_I2C
	return false;
#endif
	for(i = 1; i < 65; i++) {
		uint8_t romByte = (i - 1) >> 3;
		uint8_t romBit = 1 << ((i - 1) & 7);

		if (i < searchLastDisrepancy) {
			// Follow the path of the previous search
			direction = (searchAddress[romByte] & romBit) ? 1 : 0;
		} else {
			/* If i == searchLastDisrepancy, we take the '1' path to
				find the next device */
			// If i > searchLastDisrepancy, we take the '0' path (default start)
			direction = (i == searchLastDisrepancy);
		}

		busyWait();
		if (last_err != ERR_NONE) {
			return false;
		}
		_write_cmd(DS2482_CMD_1WIRE_TRIPLET, direction ? 0x80 : 0);
		stat = busyWait();

		if (last_err != ERR_NONE)
			return false;

		uint8_t id = stat & DS2482_STATUS_SBR;
		uint8_t comp_id = stat & DS2482_STATUS_TSB;
		direction = stat & DS2482_STATUS_DIR; // The DS2482 tells us which way it actually went

		if (id != 0 && comp_id != 0) {
			// no devices on 1-wire
			return false;
		} else {
			// If both paths were available (0 and 1), and we took the 0 path,
			// record this as a discrepancy for the next search iteration.
			if (id == 0 && comp_id == 0 && direction == 0) {
				last_zero = i;
				// Track the last discrepancy within the family code (first 8 bits)
				if (last_zero < 9)
					searchLastFamilyDiscrepancy = last_zero;
			}
		}

		// Update the address buffer with the actual bit found
		if (direction)
			searchAddress[romByte] |= romBit;
		else
			searchAddress[romByte] &= (uint8_t)~romBit;
		/* if only interested in max 2 bytes we can stop here
		   could save 50 ms
		if (!search_mode && i == 16)
			break;
		*/
	}

	searchLastDisrepancy = last_zero;

	// If no more branches exist, or we've finished the targeted family
	if (searchLastDisrepancy == 0)
		searchExhausted = 1;

	for (i = 0; i < 8; i++) {
		newAddr[i] = searchAddress[i];
		log_event(STATE_SEARCH, searchAddress[i]);
	}
	return true;
}

void DS2482::write(const uint8_t *buf, uint16_t count, uint8_t power/* = 0 */)
{
	(void)power;
	for (uint16_t i = 0 ; i < count ; i++)
		write(buf[i]);

}

void DS2482::read(uint8_t *buf, uint16_t count) {
	for (uint16_t i = 0 ; i < count ; i++)
		buf[i] = read();
}
