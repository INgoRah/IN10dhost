#ifndef _OW_DEV_H
#define _OW_DEV_H

#include "fs.h"
#include "interface/devices.h"

using std::string;
using json = nlohmann::json;

#define ARDUINO 0xAD

class OwDev : IFs, public IDev {
	protected:
		DS2482 *ow;
		// ARDUINO or default DS2408 behavior
		int mode;
		string info;
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
		//std::map<string, OwAttribute> attributes;
		bool operator==(const OwDev& other) const {
			return rom == other.rom;
		};
		virtual void set_mode(int mode);
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
