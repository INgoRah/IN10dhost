#ifndef _OW_DEV_H
#define _OW_DEV_H

#include "fs.h"
#include "interface/devices.h"

using std::string;
using json = nlohmann::json;
using HrClock = std::chrono::high_resolution_clock;

#define ARDUINO 0xAD

enum dev_states {
	DS_CREATED = 0,
	DS_INIT = 1,
	DS_RUNNING = 2,
	DS_STALE = 3,
	DS_INVALID = 0xff
};

class OwDev : IFs, public IDev {
	protected:
		DS2482 *ds;
		// Default-initialized (not just set in the OwDev(string) ctor):
		// devices created via the default ctor + from_json() (the
		// load() path) must also start out well-defined, or init()'s
		// and begin()'s once-only guards below would read garbage.
		int state = DS_CREATED;
		string info;
		HrClock::time_point last_poll;
		// polling interval in seconds, 0 means no polling, default is 0
		uint16_t poll_interval;
		uint8_t crc8(const uint8_t *addr, uint8_t len);
	public:
		// Timing check ("is it due?"), shared by every device type. When
		// due, it also advances last_poll to schedule the next poll, so it
		// may only be called once per cycle. Not virtual: derived classes
		// never need to call or re-check their own parent class's poll()
		// by name. OwDevices::dev_poll() calls this to decide whether to
		// call poll() at all; poll() is only ever called once poll_check()
		// has just returned 1, so poll() implementations never need to
		// check it themselves.
		int poll_check();
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
		/* Called once after a device has been scanned or loaded from
		 * config, before that device's begin() runs. No hardware access
		 * here - only config/state normalization (e.g. update()).
		 * Idempotent: guarded by `state`, safe to call again. */
		virtual void init();
		/* Start hardware access and initialize its config and data.
		 * Idempotent: guarded by `state`, safe to call again on an
		 * already-running device (e.g. from a live bus rescan that
		 * revisits devices it already brought up). */
		virtual void begin(DS2482 *ds, bool soft=false);
		virtual void begin(bool soft=false) { (void)soft; };
		bool operator==(const OwDev& other) const {
			return rom == other.rom;
		};
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
