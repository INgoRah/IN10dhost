#ifndef _OWDEVICES_H
#define _OWDEVICES_H

#include <vector>
#include <chrono>
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
    int id;
    int dev_count;
    std::vector<OwDev*> devices;
};

struct Config {
    int version;
	/* poll interval in secs or 0 for no polling */
	int poll;
    int bus_count;
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
		DS2482 *ds;
#if 0
		uint8_t	pio_data[MAX_BUS][MAX_ADR];
		uint8_t dev_vers[MAX_BUS][MAX_ADR];
#endif
		HrClock::time_point last_sec;
		// independent cadence tracker for alarm_poll(), driven by
		// cache.poll rather than the fixed 1-second tick last_sec uses
		HrClock::time_point last_alarm_poll;
		void init_busses();

	public:
		OwDevices(DS2482 *ds) { this->ds = ds; }
		~OwDevices();
		void begin(bool soft=false);
		void init();
		void cacheInit();
		void load(const std::string& path);
		void save(const std::string& path);

		void search(bool mode);

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
		int bus_count() const { return MAX_BUS; }
		void set_poll(int poll) { cache.poll = poll; };
		int get_poll() { return cache.poll; };

		std::vector<OwDev*> list_devices(int bus);

		int log_dump(char* buf, size_t size);
		bool alarmHandler(uint8_t busNr);
#if 0
		uint8_t getVersion(uint8_t bus, uint8_t id);
		void versionUpdate(uint8_t bus, uint8_t id);
#endif
};

#endif