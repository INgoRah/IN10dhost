#pragma once

#include "nlohmann/json.hpp"
#include "logger.h"
#include "interface/devices.h"

using json = nlohmann::json;
using std::string;

enum action_code {
	CONFIG_LOAD = 0,
	CONFIG_SAVE = 1,
	INITIALIZED = 2,
	PERIODIC_SECOND = 3,
	PERIODIC_MINUTE = 4,
	ALARM_BEFORE = 5,
	ALARM_AFTER = 6
};
class Plugin {
public:
	string name;
	std::filesystem::path lib_path;
	void* handle;

	virtual ~Plugin() {}
	virtual void init(ILogger* logger, IDevices *devices) = 0;
	// Called when the plugin is being unloaded
	virtual void exit() = 0;
	virtual void info() = 0;
	/* called on various events
	   action codes:
		0 - config load
		1 - config save
		2 - initialized (after init and plugins_init)
		3 - every second (for regular tasks)
		4 - every minute
		5 - on alarm (before default handling), val = bus number
	     returns 0 for ok and no action
			< 0 for error
			> 0 for more action needed (e.g. updating)
		6 - on alarm (after default handling), val = bus number
		 returns:
			1 - for error, no further action, abort
			0 - ok, nothing done and continue processing alarms (default)
			1 - Handled, continue processing alarms
			2 - Handled and no more action allowed, stop processing alarms
	*/
	virtual int action(int action, int val) = 0;

	// config data set and get...
	virtual json config_get() const = 0;
	virtual void config_set(const json& j) = 0;
};

// Define the signature of the factory function
typedef Plugin* (*create_t)();
typedef void (*destroy_t)(Plugin*);
