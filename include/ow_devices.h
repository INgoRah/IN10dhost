#ifndef _OWDEVICES_H
#define _OWDEVICES_H

#include <vector>
#include <chrono>
#include <mutex>
#include "nlohmann/json.hpp"
#include "ds2482.h"
#include "ow_dev.h"

// Must come before switch_handler.h: its MAX_SWITCHES depends on
// MAX_BUS already being defined at the point it's included.
#ifndef MAX_BUS
#define MAX_BUS 4
#endif

#include "switch_handler.h"
#include "interface/devices.h"

using std::string;
using json = nlohmann::json;

using HrClock = std::chrono::high_resolution_clock;

struct Bus {
    int id = 0;
    int dev_count = 0;
    std::vector<OwDev*> devices;
};

struct Config {
    int version = 0;
	/* poll interval in secs or 0 for no polling */
	int poll = 0;
    int bus_count = 0;
	/* seconds between saves of the config, 0: only on demand
	   (settings/save) and at shutdown */
	int save_interval = 0;
	/* seconds between two searches of the busses with a stale device,
	   0: none, see OwDevices::presence_check() */
	int presence = 300;
    std::vector<std::unique_ptr<OwDev>> devices;
	std::vector<Bus> busses;
	// TODO move to switch handler
	// to avoid declarating this struct here
	// use plugin for switch handling and store the config there
	std::vector<struct _sw_tbl> switches;
};

extern Config cache;

class OwDevices : public IDevices
{
	private:
		DS2482 *ds = nullptr;
#if 0
		uint8_t	pio_data[MAX_BUS][MAX_ADR];
		uint8_t dev_vers[MAX_BUS][MAX_ADR];
#endif
		HrClock::time_point last_sec;
		// independent cadence tracker for alarm_poll(), driven by
		// cache.poll rather than the fixed 1-second tick last_sec uses
		HrClock::time_point last_alarm_poll;
		void init_busses();
		// the config file load() read, where save() writes to
		std::string data_path;
		// one save at a time: the timer (poll worker) and settings/save
		// (FUSE thread) may meet
		std::mutex save_mtx;
		HrClock::time_point last_save_tick;
		// wall clock time of the last successful save, for status/health
		std::chrono::system_clock::time_point saved_at{};
		// where log_save() puts the 1-Wire traces, see set_log_dir()
		std::string log_dir;
		// one trace save at a time, and when the last one was written
		std::mutex log_mtx;
		HrClock::time_point last_log_save{};
		// the last presence_check() by dev_poll(), see Config::presence
		HrClock::time_point last_presence{};
		/* Search of one bus, full or (cond) a conditional one, the ROMs
		   found go to found. False when the bus could not be searched
		   (no such bus, an error on the way): then nothing can be said
		   about who is missing. */
		bool scan_bus(int bus, std::vector<uint64_t>& found, bool cond = false);

	public:
		OwDevices(DS2482 *ds) { this->ds = ds; }
		~OwDevices();
		void begin(bool soft=false);
		void init();
		void cacheInit();
		void load(const std::string& path);
		/* Writes the config to path, safely: to "<path>.tmp" first,
		   synced to disk, then renamed over path - a crash or power loss
		   leaves the old or the new file, never a broken one. Returns 0,
		   or a negative errno. */
		int save(const std::string& path);
		// the same to the config file in use, see set_data_path()
		int save();
		void set_data_path(const std::string& path) { data_path = path; }
		const std::string& get_data_path() const { return data_path; }
		// the save timer: saves when save_interval seconds passed since
		// the last save, called once a second. Returns 1 when it saved.
		int save_poll(HrClock::time_point now = HrClock::now());
		void set_save_interval(int secs) { cache.save_interval = secs; }
		int get_save_interval() const { return cache.save_interval; }
		void set_presence(int secs) { cache.presence = secs; }
		int get_presence() const { return cache.presence; }
		// when the config was last saved, epoch while it was not yet
		std::chrono::system_clock::time_point last_saved();

		/* mode true: a full search of all busses, adds the new devices
		   and marks the missing ones stale, see presence(). Returns the
		   known devices found.
		   mode false: a conditional search, for /uncached/alarm.
		   Returns the known devices in alarm now - as the bus has it,
		   not the alarm flags of the alarm handling (/alarm). Nothing
		   is handled nor are latches read. */
		std::vector<OwDev*> search(bool mode);
		/* The devices of bus found by a search are running, the others
		   stale - except the ones a 1-Wire search cannot see (the I2C
		   Arduino). */
		void presence(int bus, const std::vector<uint64_t>& found);
		/* Searches the busses with a stale device (all with all=true)
		   and marks who is there, see presence(). An unplugged device
		   is no reason for a search. Adds no new devices, that is
		   search()'s job. */
		void presence_check(bool all = false);

		void update_device(int bus, string rom);
		void add_device(OwDev* dev);
		void update_data();
		/** Returns the minimun time in ms for the next poll action
		 *  @return
		 *   time in ms for the next poll action, or
		 *   0 for poll now or
		 *  -1 for no polling
		 */
		int poll_time();
		// Per-device polling: runs the once-a-second plugin tick and
		// walks cache.devices calling each dev->poll() on its own
		// poll_interval-driven cycle (see poll_time()).
		int dev_poll();
		// Global, cache.poll-driven alarm polling: the periodic
		// counterpart to GPIO-interrupt-driven alarm detection, for
		// setups without (or in addition to) a wired interrupt line.
		// No-op when cache.poll <= 0 or SwitchHandler's own
		// MODE_ALRAM_POLLING bit is off.
		int alarm_poll();

		OwDev* find(const string& rom);
		OwDev* find(uint64_t targetCode);
		IDev* get_dev(uint64_t targetCode);
		OwDev* find(uint8_t bus, uint8_t id, uint8_t type = 0x29);
		/* The other ROMs of the same chip: an owSlave built with
		   DUAL_ROM answers under two family codes with the same serial
		   (e.g. 20.0200FDFF667734 and 29.0200FDFF6677F8). */
		std::vector<OwDev*> twins(const OwDev* dev);
		int bus_count() const { return MAX_BUS; }
		void set_poll(int poll) { cache.poll = poll; };
		int get_poll() { return cache.poll; };

		std::vector<OwDev*> list_devices(int bus);
		// IDevices: the pins with a role across all buses
		std::vector<PinInfo> pins(PinRole role = PinRole::none) override;
		int role_switch(PinRole role, bool on) override;

		int log_dump(char* buf, size_t size);
		/* Writes the 1-Wire trace (status/1wire.vcd) to
		   <log dir>/1wire-<YYYYMMDD-HHMMSS>.vcd, so it survives a
		   restart. Only the newest LOG_KEEP traces are kept. Unless
		   forced, a save within LOG_SAVE_MIN_SECS of the last one is
		   skipped: a device stuck in a watchdog loop must not flood
		   the disk. Returns the file written, empty if none. */
		std::string log_save(bool force = true);
		/* the directory for log_save(); falls back to $HOME when it
		   cannot be created */
		void set_log_dir(const std::string& dir) { log_dir = dir; }
		const std::string& get_log_dir() const { return log_dir; }
		bool alarmHandler(uint8_t busNr, uint8_t target = 0x29);
#if 0
		uint8_t getVersion(uint8_t bus, uint8_t id);
		void versionUpdate(uint8_t bus, uint8_t id);
#endif
};

#endif