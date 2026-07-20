#include <fstream>
#include <iostream>
#include <string>
#include <algorithm> // for std::max,min...
#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "ds1820.h"
#include "ds2408.h"
#include "ds2450.h"
#include "ard_i2c.h"
#include "plugins.h"

#include "nlohmann/json.hpp"

using json = nlohmann::json;

Plugins plugins;
Config cache;

struct Device {
	long long unsigned int romCode;
	OwDev* dev;
};

std::array<Device, 30> dev_list;
size_t deviceCount = 0;

/* part of switch handler */
extern void to_json(json& j, const _sw_tbl& b);
extern void from_json(const json& j, _sw_tbl& b);

void to_json(json& j, const Bus& b) {
	j = json{
		{"id", b.id},
		{"dev_count", b.dev_count}
	};
}

void from_json(const json& j, Bus& b) {
	try {
		j.at("id").get_to(b.id);
		j.at("dev_count").get_to(b.dev_count);
	}
	catch (const std::exception& e) {
		printf("cannot read bus\n");
	}
}

void to_json(json& j, const Config& c) {
	j = json {
		{"version", 1},
		{"mode", c.mode},
		{"poll", c.poll},
		{"log", c.log},
		{"bus_count", c.bus_count},
		{"busses", c.busses},
		{"switches", c.switches}
	};
	j["devices"] = json::array();
	for (const auto& dev : c.devices) {
		j["devices"].push_back(dev->to_json());
	}
}

/* =========================
   Factory (polymorphic creation)
   ========================= */

#define RETURN_DEVICE(type_str, dev_type) \
	do { \
		if (type == (type_str)) { \
			auto dev = std::make_unique<dev_type>(); \
			if (dev) \
				dev->from_json(j); \
			return dev; \
		} \
	} while (0)

std::unique_ptr<OwDev> make_device_from_json(const json& j) {
	const std::string type = j.at("type").get<std::string>();

	RETURN_DEVICE("ds2408", ds2408);
	RETURN_DEVICE("ds1820", ds1820);
	RETURN_DEVICE("ds2450", ds2450);
	RETURN_DEVICE("ard_i2c", Ard_i2c);
	// coming here means that the device is not supported
	throw std::runtime_error("Unknown device type: " + type);
}

void from_json(const json& j, Config& c) {
	c.version = j.at("version").get<int>();
	c.bus_count = j.at("bus_count").get<int>();
	j.at("busses").get_to(c.busses);
	if (j.contains("switches"))
		j.at("switches").get_to(c.switches);
	c.devices.clear();

	for (const auto& jdev : j.at("devices")) {
		c.devices.push_back(make_device_from_json(jdev));
	}
	c.mode = j.at("mode").get<int>();
	if (j.contains("log"))
		c.log = j.at("log").get<int>();
	if (j.contains("poll"))
		c.poll = j.at("poll").get<int>();
}

OwDevices::~OwDevices() {
	plugins.cleanup();
}

void OwDevices::init()
{
	cache.version = 1;
	for (size_t i = 0; i < dev_list.size(); ++i) {
		dev_list[i].romCode = 0;
		dev_list[i].dev = nullptr;
	}
	deviceCount = 0;
	cache.devices.clear();
	init_busses();
	last_sec = HrClock::now();;
}

void OwDevices::init_busses()
{
	cache.busses.clear();
	cache.bus_count = MAX_BUS;
	for (int bus = 0;bus < MAX_BUS; bus++)
		cache.busses.push_back(Bus{bus, 0, {}});
}

void OwDevices::set_mode(int mode)
{
	_mode = mode;
	cache.mode = mode;

	for (auto& dev : cache.devices)
		dev->set_mode(mode);
}

void OwDevices::set_log(int level)
{
	cache.log = level;
}

void OwDevices::load(const std::string& path) {
	json j;
	std::ifstream file(path);
	if (!file) {
		throw std::runtime_error("Cannot open config file: " + path);
	} else {
		file >> j;
		cache = j.get<Config>();
		if (cache.bus_count != MAX_BUS)
			init_busses();
		if (j.contains("plugins")) {
			plugins.load(j["plugins"]);
		}
		logger.info(std::format("Loaded config: version {}, devices {}",
			cache.version,
			(unsigned int)cache.devices.size()));
	}
	logger.set_level((LogLevel)cache.log);
	for (auto& dev : cache.devices) {
		dev->update();
	}
	update_data();
	plugins.action(2, 0); // loaded
}

void OwDevices::save(const std::string& path) {
	std::ofstream file(path);
	cache.version = 1;
	// this is just for information and not used in the system
	for (auto& b : cache.busses)
		b.dev_count = b.devices.size();
	json j = cache;
	try {
		json j_plugins = plugins.save();
		j["plugins"] = j_plugins;
	}
	catch (const std::exception& e) {
		logger.error("Cannot save plugins");
	}
	file << j.dump(4); // pretty-print with 4-space indentation
}

void OwDevices::add_device(OwDev* dev)
{
	// find position where it belongs
	auto it = std::lower_bound(dev_list.begin(), dev_list.begin() + deviceCount, dev->rom_code,
		[](const Device& d, uint64_t code) {
			return d.romCode < code;
		});
	// check if it already exists
	if (it != dev_list.begin() + deviceCount && it->romCode == dev->rom_code) {
		// exists
		return;
	}

	// Array full ?
	if (deviceCount >= dev_list.size()) return;
	// Put element at the right position
	// pretty fast with 30 elements
	size_t index = std::distance(dev_list.begin(), it);
	if (index < deviceCount) {
		std::move_backward(dev_list.begin() + index, dev_list.begin() + deviceCount,
						   dev_list.begin() + deviceCount + 1);
	}

	dev_list[index].romCode = dev->rom_code;
	dev_list[index].dev = dev;

	deviceCount++;
}

OwDev* OwDevices::find(uint64_t targetCode) {
	auto it = std::lower_bound(dev_list.begin(), dev_list.begin() + deviceCount, targetCode,
		[](const Device& d, uint64_t code) {
			return d.romCode < code;
		});

	if (it != dev_list.begin() + deviceCount && it->romCode == targetCode) {
		OwDev* dev = it->dev;
		if (dev)
			return dev;
	}
	return nullptr;
}

OwDev* OwDevices::find(const string& rom) {
	string s = rom;
	if (rom.find(".") == 2)
		s = s.erase(2, 1);
	long long unsigned int v = std::strtoull(s.c_str(), nullptr, 16);
	return find(v);
}

OwDev* OwDevices::find(uint8_t bus, uint8_t id, uint8_t type)
{
	for (size_t i = 0; i < deviceCount; ++i) {
		if (((dev_list[i].romCode >> (6 * 8)) & 0xFF) == id) {
			OwDev* dev = dev_list[i].dev;
			if (dev->bus == bus) {
				if (type == 0xff)
					return dev;
				else if (((dev_list[i].romCode >> (7 * 8)) & 0xFF) == type)
					return dev;
			}
		}
	}
	return nullptr;
}

IDev* OwDevices::get_dev(uint64_t targetCode)
{
	OwDev* dev = find(targetCode);
	logger.info(std::format("get_dev for code {:012X} found dev {}", targetCode, dev ? dev->rom : "null"));
	return static_cast<IDev*>(dev);
}

// called after scan or load to
// update the caches
void OwDevices::update_data()
{
	int bus;
	if (cache.busses.size() != MAX_BUS)
		init_busses();
	for (bus = 0;bus < MAX_BUS; bus++)
		cache.busses[bus].devices.clear();
	deviceCount = 0;
	logger.log(LogLevel::DEBUG, std::to_string(cache.devices.size()) + " devices ");
	for (auto it = cache.devices.begin(); it != cache.devices.end(); ) {
		auto& dev = *it;

		if (find(dev->rom_code)) {
			logger.warn(std::format("duplicated dev {} with {}", dev->rom, dev->rom_code));
			it = cache.devices.erase(it); // Erase returns the NEXT valid iterator
			continue;
		}
		cache.busses[dev->bus].devices.push_back(it->get());
		add_device(it->get());
		dev->set_mode(cache.mode);
		++it; // Only increment if we didn't erase
	}
	logger.log(LogLevel::DEBUG, "update, busses=" + std::to_string(cache.busses.size()) + " = " + std::to_string(deviceCount) + " devices ");
}

// called after scan to creat a device if needed and
// add it to the caches
void OwDevices::update_device(int bus, string rom) {
	if (find(rom) != nullptr) {
		logger.verbose(std::format("device {} already found", rom));
		return;
	}
	OwDev* dev = nullptr;
	// "factory" for different devices
	if (rom.substr(0, 2) == "29") {
		auto& p = cache.devices.emplace_back(std::make_unique<ds2408>(rom));
		dev = p.get();
	}
	if (rom.substr(0, 2) == "28") {
		auto& p = cache.devices.emplace_back(std::make_unique<ds1820>(rom));
		dev = p.get();
	}
	if (rom.substr(0, 2) == "20") {
		auto& p = cache.devices.emplace_back(std::make_unique<ds2450>(rom));
		dev = p.get();
	}
	if (rom.substr(0, 2) == "AD") {
		auto& p = cache.devices.emplace_back(std::make_unique<Ard_i2c>(rom));
		dev = p.get();
	}
	if (dev) {
		dev->bus = bus;
		dev->begin(ow);
		dev->set_mode(_mode);
		logger.verbose(std::format("adding device {}", dev->rom));
		add_device(dev);
		// TODO update the bus cache as well
	} else {
		printf("unknown device %s\n", rom.c_str());
		// TODO create generic device to at least show it in the list
		// issue ...
	}
}

std::vector<OwDev*> OwDevices::list_devices(int bus)
{
	return cache.busses[bus].devices;
}

// search devs
uint8_t OwDevices::search(bool mode)
{
	char buf[18];
	int pos;
	uint8_t adr[8], bus;
#ifdef USE_I2C
	uint8_t  res = 0;
#endif

	for (bus = 0; bus < 4; bus++) {
		std::lock_guard<std::mutex> lock(ow->mtx);
		ow->selectChannel(bus);
		ow->reset_search();
#ifdef USE_I2C
		while (ow->search(adr, mode)) {
				res++;
				sprintf(buf, "%02X.", adr[0]);
				pos = 3;
				for (int j = 1; j < 8; j++) {
					sprintf(&buf[pos], "%02X", adr[j]);
					pos += 2;
				}
				assert (pos < (int)sizeof(buf));
				update_device(bus, buf);
		}
#endif
	}
#ifndef USE_I2C
	(void)mode;
	uint8_t  adrt[8] = { 0x28, 0x5, 0x1, 0xFA, 0xFE, 0x66, 0x77, 0xC6};
	sprintf(buf, "%02X.", adrt[0]);
	pos = 2;
	for (int j = 1; j < 8; j++) {
		sprintf(&buf[pos], "%02X", adr[j]);
		pos += 2;
	}
	update_device(1, buf);
	update_device(0, "29.0200FDFF6677F8");
	update_device(0, "29.0500FAFF6677FB");
	update_device(1, "29.0701F8FE6677F4");
	update_device(1, "29.0701F8FE6677F5");
#endif
	// dummy device for arduino
	update_device(0, "AD.0900F8FF6677E2");
	update_data();

	return 1;
}

void OwDevices::begin(DS2482 *ds)
{
	ow = ds;
	ow->log_init("ds2482.vcd");

	for (auto& dev : cache.devices) {
		dev->begin(ds);
	}
#ifdef USE_I2C
    if (!ow->init()) {
        printf("Failed to initialize DS2482\n");
		return;
	} else {
		printf("Initialized DS2482\n");
	}
#endif
#ifdef USE_I2C_EXCLUSIVE
	ow->resetDev();
	ow->configureDev(DS2482_CONFIG_APU);
#endif
}

int OwDevices::poll_time()
{
	int next_timeout = 60 * 1000; // 60 seconds
	auto now = HrClock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sec).count();
	if (elapsed >= 1000) {
		// time to poll now
		return 0;
	}
	// return remaining time in ms for the second timer
	int sec = (int)(1000 - elapsed);
	// check all devices for polling
#if 1
	for (auto& dev : cache.devices) {
		int next = dev->poll_next();
		if (next == 0)
			return 0; // time to poll now
		if (next > 0 && next < next_timeout)
	        next_timeout = std::min(next_timeout, next);
    }
#endif
	if (next_timeout == 60 * 1000)
		// no device needs polling, return the seconds
		return sec;
	return std::min(next_timeout, sec);
}

int OwDevices::poll()
{
	int ret = -1;
	int cnt = 0;
	// check if seconds timer is up
	auto now = HrClock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sec).count();
	if (elapsed >= 1000) {
		last_sec = now;
		cnt = plugins.action(PERIODIC_SECOND);
	}
    for (auto& dev : cache.devices) {
		int poll = dev->poll();
		if (poll == 1)
			cnt++; // at least one device polled
		else if (poll == 0)
			ret = std::max(ret, 0);
	}
	if (cnt == 0)
		return ret; // no device needs polling
	return cnt;
}