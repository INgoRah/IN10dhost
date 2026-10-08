#ifndef _OW_DEV_H
#define _OW_DEV_H

#include <atomic>
#include <mutex>
#include <vector>
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
	private:
		// Guards the members of this device - here and in every subclass
		// (e.g. ds2408::data[]) - against the FUSE threads, the poll
		// worker and plugins using it at the same time. Take it with
		// lock(). Recursive, because the handlers call each other
		// (w_pio() -> pin_switch() -> pio_set()). Always taken before
		// the bus mutex (ds->mtx), never while holding it.
		mutable std::recursive_mutex dev_mtx;
		mutable int lock_depth = 0;
		// ACT_DEV_CHANGE payloads raised while locked, see notify_change()
		mutable std::vector<json> pending;
	protected:
		/* Tells the plugins this device changed. While the device is
		   locked the event is only queued, and sent by the outermost
		   DevLock once the lock is released: a plugin reacting to it may
		   use the device from another thread (jsengine runs scripts under
		   its own lock), which would deadlock against a held lock. */
		void notify_change(json data) const;
		// nullptr until begin() runs; a device method that dereferences
		// it without begin() having run first is a bug in that caller,
		// not something to paper over here, but the pointer itself must
		// still start out well-defined for the same reason state does.
		DS2482 *ds = nullptr;
		// Default-initialized (not just set in the OwDev(string) ctor):
		// devices created via the default ctor + from_json() (the
		// load() path) must also start out well-defined, or init()'s
		// and begin()'s once-only guards below would read garbage.
		int state = DS_CREATED;
		string info;
		HrClock::time_point last_poll;
		// polling interval in seconds, 0 means no polling, default is 0
		uint16_t poll_interval = 0;
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
		uint64_t rom_code = 0;
		string type;
		string name;
		uint8_t addr[8] = {};
		int id = 0;
		int bus = 0;
		// set by the alarm handling (SwitchHandler::dev_alarm()), listed
		// under /alarm until cleared - see ds2408::w_latched(). Atomic
		// as /alarm is listed without taking the device lock.
		std::atomic<bool> alarm{false};

		/* Scoped device lock, see dev_mtx. Sends the queued change
		   events when the outermost one goes out of scope. */
		class DevLock {
			const OwDev& dev;
		public:
			explicit DevLock(const OwDev& d);
			~DevLock();
			DevLock(const DevLock&) = delete;
			DevLock& operator=(const DevLock&) = delete;
		};
		DevLock lock() const { return DevLock(*this); }

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
		// the name given to the device, or its rom while it has none
		std::string display_name() const { auto lk = lock(); return name.empty() ? rom : name; };
		// copies taken under the device lock, see IDev
		std::string get_name() const override { return display_name(); };
		std::string get_type() const override { auto lk = lock(); return type; };
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
