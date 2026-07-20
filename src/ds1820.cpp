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

static struct filetype DS18S20[] = {
	{ "temperature", 5 },
	{ "humidity", 4 }
};

std::vector<std::string> ds1820::fs_dir(string& path) const
{
	// add standards
	std::vector<std::string> dir = OwDev::fs_dir(path);

	for (const auto& s : DS18S20)
		dir.push_back(s.name);

	return dir;
}

int ds1820::fs_attr(std::string& path) const
{
	for (const auto& s : DS18S20) {
		if (path.find(s.name) != std::string::npos) {
			return s.suglen;
		}
	}

	// add standards
	return OwDev::fs_attr(path);
}

int ds1820::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	if (path.find("humidity") != string::npos) {
		std::sprintf(buf, "%d", hum);
		return std::strlen(buf);
	}
	if (path.find("temperature") != string::npos) {
		//std::unique_lock<std::mutex> mtx(ow->mtx, std::defer_lock);
		//std::unique_lock<std::mutex> mtx(ow->mtx);
		if (uncached) {
			temp_read(0);
			//logger.info ("#1 reading %s/%s %d ...\n", rom.c_str(), path.c_str(), ret);
			temp_read(1);
			logger.info ("DS1820 reading temp " +  std::to_string(temp) + "° " + std::to_string(hum) + " %");
		}
		std::sprintf(buf, "%1.2f", temp);
		return std::strlen(buf);
	}
	return OwDev::fs_read(path, buf, size, uncached);
}

/**
 * Returns temperature raw. celsius = tempRead * 16
 * mode 0: start conversion
 * 1: set alarms and reset
 * 2: read temperature and scratchpad
 * No retry handling because the data might not so important as next cycle
 * will come
*/
float ds1820::temp_read(const uint8_t mode)
{
	bool ret;
	uint8_t scratchPad[9];
	(void)mode;

	std::lock_guard<std::mutex> m(ow->mtx);
	ret = ow->selectChannel(bus);
	if (!ret) {
		return -1;
	}
	ow->reset();
	ow->select(addr);
	switch (mode) {
	case 0:
		ow->write(STARTCONVO);
		return 0;
	case 2:
		ow->write(WRITESCRATCH);
		ow->write(35); // high alarm temp
		ow->write(10); // low alarm temp
		return 0;
	case 1:
	default:
		/* read temp */
		break;
	}
	ow->write(READSCRATCH);
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
		scratchPad[i] = ow->read();
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

int ds1820::poll()
{
	int poll = OwDev::poll();
	if (poll == 1) {
		float old = temp;
		temp_read(0);
		//logger.info ("#1 reading %s/%s %d ...\n", rom.c_str(), path.c_str(), ret);
		temp_read(1);
		if (temp != old)
			logger.info (std::format("{} temp={} °, hum={} %", rom, temp, hum));
		return 1;
	}
	return poll;
}
