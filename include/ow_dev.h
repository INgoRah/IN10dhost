#ifndef _OW_DEV_H
#define _OW_DEV_H

#include "fs.h"
#include "interface/devices.h"

using std::string;
using json = nlohmann::json;
using HrClock = std::chrono::high_resolution_clock;

#define ARDUINO 0xAD

class OwDev : IFs, public IDev {
	protected:
		DS2482 *ow;
		// ARDUINO or default DS2408 behavior
		int mode;
		string info;
		HrClock::time_point last_poll;
		// polling interval in seconds, 0 means no polling, default is 0
		uint16_t poll_interval;
		uint8_t crc8(const uint8_t *addr, uint8_t len);
	public:
		string rom;
		uint64_t rom_code;
		string type;
		string name;
		uint8_t addr[8];
		int id;
		int bus;
		bool alarm;
		OwDev() { };
		OwDev(string rom);
		virtual ~OwDev() {};

		void update();
		void begin(DS2482 *ds);
		bool operator==(const OwDev& other) const {
			return rom == other.rom;
		};
		virtual void set_mode(int mode);
		// called periodically to update the device state,
		// e.g. for polling or timed actions
		// the device calls is responsible to check the time and decide if it
		// needs to do something and then perform the action(s)
		virtual int poll();
		virtual int poll_next();
		// IDev functions
		const char* get_name() const { return name.c_str(); };
		const char* get_type() const { return type.c_str(); };
		// IFs functions
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_open(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;

		virtual void from_json(const json& j);
		virtual json to_json() const;
};
#endif /* _OW_DEV_H */
