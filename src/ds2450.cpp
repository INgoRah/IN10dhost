#include <fstream>
#include <iostream>
#include <string>
#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "ds2450.h"

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
static struct filetype DS2450[] = {
	{ "volt.A", 4 },
	{ "volt.B", 4 },
	{ "volt.C", 4 },
	{ "volt.D", 4 }
};

std::vector<std::string> ds2450::fs_dir(string& path) const
{
	// add standards
	std::vector<std::string> dir = OwDev::fs_dir(path);

	for (const auto& s : DS2450)
		dir.push_back(s.name);

	return dir;
}

int ds2450::fs_attr(std::string& path) const
{
	for (const auto& s : DS2450) {
		if (path.find(s.name) != std::string::npos) {
			return s.suglen;
		}
	}
	// add standards
	return OwDev::fs_attr(path);
}

int ds2450::fs_read_volt(uint8_t ch, char* buf, bool uncached)
{
	float volt_x = 0.0f;

	if (uncached) {
		uint16_t volt;
		if (adc_read(ch, 0) != 0)
			return EAGAIN;
		volt = adc_read(ch, 1);
		logger.info ("DS2450 reading raw=" + std::to_string(volt));
		volt_x = (volt * 5.0 / 1024);
		switch (ch) {
			case 0: volt_a = volt_x; break;
			case 1: volt_b = volt_x; break;
			case 2: volt_c = volt_x; break;
			case 3: volt_d = volt_x; break;
		}
	} else {
		switch (ch) {
			case 0: volt_x = volt_a; break;
			case 1: volt_x = volt_b; break;
			case 2: volt_x = volt_c; break;
			case 3: volt_x = volt_d; break;
		}
	}
	std::sprintf(buf, "%1.2f", volt_x);

	return std::strlen(buf);
}

int ds2450::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	uint8_t ch = 0xff;
	if (path.find("volt.A") != string::npos)
		ch = 0;
	else if (path.find("volt.B") != string::npos)
		ch = 1;
	else if (path.find("volt.C") != string::npos)
		ch = 2;
	else if (path.find("volt.D") != string::npos)
		ch = 3;
	if (ch != 0xff) {
		if (size < 5)
			return -EINVAL;
		return fs_read_volt(ch, buf, uncached);
	}
	return OwDev::fs_read(path, buf, size, uncached);
}

int16_t ds2450::adc_read(uint8_t ch, uint8_t flag)
{
	if (ch > 3)
		return -1;
	if (flag == 2)
		return dat[ch];
	bool ret;
	uint16_t crc;

	std::lock_guard<std::mutex> lock(ow->mtx);
	ret = ow->selectChannel(bus);
	if (!ret)
		return -1;
	ow->reset();
	ow->select(addr);
	if (flag == 0) {
		ow->write(0x3c);
		ow->write(ch+1);
		// clear all
		ow->write(0x55);
		crc = ow->read();
		crc |= ow->read() << 8;
		return 0;
	}
	ow->write(0xAA);
	ow->write(0x0);
	ow->write(0x0);
	for (int i = 0; i < 4; i++) {
		dat[i] = ow->read();
		dat[i] |= ow->read() << 8;
	}
	crc = ow->read();
	crc |= ow->read() << 8;

	return dat[ch];
}

int ds2450::poll()
{
	int poll = OwDev::poll();
	if (poll == 1) {
		if (adc_read(0, 0) != 0)
			return EAGAIN;
		volt_a = adc_read(0, 1);
		return 1;
	}
	return poll;
}
