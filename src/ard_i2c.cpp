#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h> // for usleep
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include <iostream>
#include <cstdint>

#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ard_i2c.h"
#include "ds2408.h"
#include "ds1820.h"
#include "switch_handler.h"

#define ARD_I2C_ADDR 0x2f

extern OwDevices ow;

enum {
	SRC_CHANGE = 0,
	DST_CHANGE = 1,
	TEMP_CHANGE = 2,
	BRIGHTNESS_CHANGE,
	DIMMING_DOWN,
	HUMIDITY_CHANGE,
	POWER_IMP = 6,
	SYS_START = 7
};

// pwr_total has never had a read/write handler (no case for it ever
// matched here, and OwDev's own fs_read/fs_write do not recognize it
// either), so its row has no callbacks - it still shows up in
// getattr()/readdir() the same as before, reads/writes still fall
// through to OwDev::fs_read()/fs_write() same as before
//
// out-of-line definition of the private static members declared in
// ard_i2c.h; this counts as class scope for access control, so it can
// take the address of the private handlers below directly
const FsEntry<Ard_i2c> Ard_i2c::table[] = {
	{ "mode", 3, 0, false, nullptr, nullptr, &Ard_i2c::r_mode, &Ard_i2c::w_mode },
	{ "power", 3, 0, false, nullptr, nullptr, &Ard_i2c::r_power, nullptr },
	{ "pwr_total", 3, 0, false, nullptr, nullptr, nullptr, nullptr },
	{ "test", 2, 0, false, nullptr, nullptr, nullptr, &Ard_i2c::w_test },
	{ "int_min", 6, 0, false, nullptr, nullptr, &Ard_i2c::r_int_min, nullptr },
	{ "int_max", 6, 0, false, nullptr, nullptr, &Ard_i2c::r_int_max, nullptr },
	{ "int_avg", 6, 0, false, nullptr, nullptr, &Ard_i2c::r_int_avg, nullptr },
};
const size_t Ard_i2c::n_table = sizeof(Ard_i2c::table) / sizeof(Ard_i2c::table[0]);

json Ard_i2c::to_json() const {
	json j = OwDev::to_json(); // Get base class fields
	j["mode"] = mode; // Add specific field

	return j;
};

void Ard_i2c::from_json(const json& j) {
	OwDev::from_json(j); // Delegate common fields to base
	if (j.contains("mode")) {
		j.at("mode").get_to(mode);
	}
}

Ard_i2c::Ard_i2c()
{
	lastSeq = 0xff;
	this->type = "ard_i2c";
	this->mode = 0;
	this->power = 0;
}

Ard_i2c::Ard_i2c(std::string rom) : OwDev(rom)
{
	// initialize members (can't delegate to default when also initializing base OwDev)
	lastSeq = 0xff;
	this->type = "ard_i2c";
	this->power = 0;
}

#ifdef USE_I2C
int i2c_write(int fd, uint8_t cmd)
{
	struct i2c_msg msg = {
		.addr = ARD_I2C_ADDR,
		.flags = 0,
		.len = 1,
		.buf = &cmd
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	return ioctl(fd, I2C_RDWR, &rdwr);
}

/* i2c read one byte */
uint8_t i2c_read(int fd)
{
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

	msgs[0].addr = ARD_I2C_ADDR;
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = msgs,
		.nmsgs = 1,
	};
	ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret < 0) {
		return 0xff;
	}

	return d;
}

int i2c_read_data(int fd, uint8_t* buf, uint16_t size) {
	struct i2c_msg msg = {
		.addr = ARD_I2C_ADDR,
		.flags = I2C_M_RD,
		.len = size,
		.buf = buf
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	return ioctl(fd, I2C_RDWR, &rdwr);
}

int i2c_write_data(int fd, uint8_t* buf, uint16_t size)
{
	struct i2c_msg msg = {
		.addr = ARD_I2C_ADDR,
		.flags = 0,
		.len = size,
		.buf = buf
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	return ioctl(fd, I2C_RDWR, &rdwr);
}
#endif

void Ard_i2c::begin(bool soft)
{
	(void)soft;
	logger.info("starting arduino");
#ifdef USE_I2C
#if 0
	// check status / fifo
	uint8_t val;
	do {
		i2c_write_data(ad_fd, 0xe2, 0xa8);
		val = i2c_read(ad_fd);
	} while ((val & 0x01) != 0);
	close(ad_fd);
#endif
#endif
	set_mode(mode);
}

void Ard_i2c::end()
{
}

void Ard_i2c::set_mode(int mode)
{
	this->mode = mode;
#ifdef USE_I2C
	int fd = open("/dev/i2c-0", O_RDWR);
	if (fd < 0) {
		printf("error opening arduino\n");
		return;
	}
	uint8_t buf[] = { 0x69, (uint8_t)(mode & 0xff) };
	i2c_write_data(fd, buf, 2);
	buf[0] = 0xE1;
	i2c_write_data(fd, buf, 1);
	close(fd);
	logger.verbose(std::format("set mode AD={:#x}", mode));
#endif
}

#ifdef USE_I2C
// Helper to mimic JS delay(ms)
void delay_ms(int ms) {
	usleep(ms * 1000);
}
#endif

void Ard_i2c::interrupt() {
#ifdef USE_I2C
	HrClock::time_point tp = HrClock::now();
	int fd = open("/dev/i2c-0", O_RDWR);

	if (fd < 0) {
		logger.warn("Arduino open failed");
		return;
	}
	if (mode == 0x10) {
		// read status which clears the gpio ("interrupts")
		uint8_t status = i2c_read(fd);
		close(fd);
		//logger.verbose(std::format("AD Stat={:#x}", status));
		if ((status & 0x3) == 0) {
			// call switch handler
			for (int bus = 0; bus < MAX_BUS; bus++)
				ow.alarmHandler(bus);
		} else {
			ow.alarmHandler((status & 0x3) - 1);
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(HrClock::now() - tp);
			if (duration < min_dur)
				min_dur = duration;
			if (duration > max_dur)
				max_dur = duration;
			sum_dur += duration;
			dur_count++;
			/*logger.info(std::format("used {} (min={} max={} avg={}ms over {} calls)",
				duration, min_dur, max_dur, sum_dur.count() / dur_count, dur_count));*/
		}

		return;
	}
#if 0
	events(fd);
#endif
	close(fd);
#endif
}

#if 0
void Ard_i2c::events(int fd, OwDevices* ow)
{
#ifdef USE_I2C
	uint8_t rbuf[10];
	uint8_t buf[2];
	int cnt;
	int err = 0;
	int act = 0;
	uint8_t stat = 0;
	do {
		if (err) {
			printf("Error case @%d...\n", act);
		}
		// act 1: check alarm status / acquire SW lock
		buf[0] = 0xE2;
		buf[1] = 0xA8;
		i2c_write_data(fd, buf, 2);
		delay_ms(1);
		// act 2: read status
		rbuf[0] = i2c_read(fd);
		act = 2;
		if (rbuf[0] == 0xff) {
			i2c_write(fd, 0xEF); // Release lock
			delay_ms(10);
			err++;
			continue;
		}
		if ((rbuf[0] & 0x40) == 0x0) {
			i2c_write(fd, 0xEF); // Release lock
			//printf("no event\n");
			return;
		}
		// act 3: request event data, CMD_EVT_DATA
		i2c_write(fd, 0x01);
		cnt = 50;
		do {
			delay_ms(1);
			// act 4: check for status STAT_BUSY = 0x1
			rbuf[0] = i2c_read(fd);
			if (rbuf[0] == 0xff)
				continue;
			// STAT_NO_DATA = 0x80
			if ((rbuf[0] & 0x80) == 0x80) {
				printf("no data / handled before? %02x\n", rbuf[0]);
				i2c_write(fd, 0xEF); // Release lock
				return;
			}
		} while (cnt-- > 0 && (rbuf[0] == 0xC0));

		act = 5;
		// Select data register
		i2c_write(fd, 0x96);
		// Read 10 bytes of event data
		if (i2c_read_data(fd, rbuf, 10) < 0) {
			err++;
			continue;
		}

		// Parse data
		uint8_t seq   = rbuf[7];
		uint8_t chk   = rbuf[8];
		stat		  = rbuf[9];

		act = 6;
		if (chk != 0xaa) {
			printf("seq received = %02x / chk = %02x\n", seq, chk);
			err++;
			continue;
		}
		// Acknowledge (ends the lock)
		buf[0] = 0x78;
		buf[1] = seq;
		i2c_write_data(fd, buf, 2);

		// Logic for sequence tracking
		if (lastSeq == 0xff) {
			lastSeq = seq;
		} else {
			uint8_t targetSeq = lastSeq + 1;
			if (targetSeq == 0x80) targetSeq = 1;
			if (targetSeq < seq) {
				printf("Sequence missing prev: %02x now: %02x\n", lastSeq, seq);
			}
			if (lastSeq == seq && err == 0) {
				printf("got this already: %02x, stat=%02x\n", lastSeq, stat);
			}
			if (err) {
				printf("%02x: Error recovered\n", lastSeq);
				err = 0;
			}
			lastSeq = seq;
		}
		printf("Event found: seq=%X ", seq);
		for (int i = 0; i < 10; i++) {
			printf("%02X ", rbuf[i]);
		}
		printf("\n");
		/*
		uint8_t bus	 = rbuf[1];
		uint8_t id	 = rbuf[2];
		uint8_t latch = rbuf[3];
		uint8_t press = rbuf[4];
		// if type = 2 -> temp = d / 16
		uint16_t d	= (rbuf[5] << 8) | rbuf[6];
		*/
		// updateOwState(t, b, a, press, latch, d); // Call your logic here
		// find by bus and device id
		OwDev* dev = owner->find(rbuf[1], rbuf[2], 0xff);
		if (dev) {
			uint8_t type = rbuf[0];
			switch (type) {
				case SRC_CHANGE:
				case DST_CHANGE:
					if (dev->addr[0] == 0x29) {
						((ds2408*)dev)->data[PIO_LS] = rbuf[3];
						((ds2408*)dev)->data[PIO_TIME] = rbuf[4];
					}
					break;
				default:
					printf("event type: %02x found device\n", type);
					printf("found device %s\n", dev->type.c_str());
					if (dev->addr[0] == 0x28 && type == 2) {
						//dev->update(d);
					}
			}
		}
		if ((stat & 0x40) == 0)
			break;
		delay_ms(10);
	} while (stat & 0x40);
#else
	(void)fd;
#endif
}
#endif

std::vector<std::string> Ard_i2c::fs_dir(string& path) const
{
	std::vector<std::string> dir = OwDev::fs_dir(path);
	std::vector<std::string> extra = fs_table::dir(*this, table, n_table, path);
	dir.insert(dir.end(), extra.begin(), extra.end());
	return dir;
}

int Ard_i2c::fs_attr(std::string& path) const
{
	int r = fs_table::attr(*this, table, n_table, path);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_attr(path);
}

int Ard_i2c::r_mode(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", mode);
	return std::strlen(buf);
}

int Ard_i2c::w_mode(const char* buf, size_t size, int)
{
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		logger.log(LogLevel::DEBUG, "write mode, set val=" + std::to_string(tmp));
		set_mode(tmp);
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int Ard_i2c::r_power(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", power);
	return std::strlen(buf);
}

int Ard_i2c::w_test(const char* buf, size_t size, int)
{
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf) & 0xff);
		logger.log(LogLevel::DEBUG, "write test, set val=" + std::to_string(tmp));
#ifdef USE_I2C
		int fd = open("/dev/i2c-0", O_RDWR);
		if (fd < 0)
			return 0;
		uint8_t out[] = { 0xde, (uint8_t)(tmp & 0xff) };
		i2c_write_data(fd, out, 2);
		close(fd);
#endif
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int Ard_i2c::r_int_min(char* buf, size_t, bool, int)
{
	// min_dur sits at its ::max() sentinel until the first sample
	std::sprintf(buf, "%d", dur_count ? (int)min_dur.count() : 0);
	return std::strlen(buf);
}

int Ard_i2c::r_int_max(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", (int)max_dur.count());
	return std::strlen(buf);
}

int Ard_i2c::r_int_avg(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", dur_count ? (int)(sum_dur.count() / dur_count) : 0);
	return std::strlen(buf);
}

int Ard_i2c::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_read(path, buf, size, uncached);
}

int Ard_i2c::fs_write(string& path, const char* buf, size_t size)
{
	int r = fs_table::write(*this, table, n_table, path, buf, size);
	if (r != fs_table::NOT_FOUND)
		return r;
	return OwDev::fs_write(path, buf, size);
}
