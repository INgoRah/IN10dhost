#pragma once

#include <filesystem>
#include <fstream>
#include "nlohmann/json.hpp"
#include "logger.h"
#include "interface/devices.h"

using json = nlohmann::json;
using std::string;

/* FNV-1a over a file's contents, used to tell a rebuilt plugin or an
   edited script from an unchanged one. Not a security check - it only
   has to notice our own rebuilds - so no crypto dependency. Unlike
   mtime it also does not fire on every scp, which rewrites timestamps
   even when the bytes are identical. Returns 0 if the file cannot be
   read. Header only so plugins can use it as well as the loader. */
inline uint64_t file_hash(const std::filesystem::path& p)
{
	std::ifstream f(p, std::ios::binary);
	uint64_t h = 0xcbf29ce484222325ULL;
	char buf[4096];

	if (!f)
		return 0;
	while (f.read(buf, sizeof(buf)) || f.gcount()) {
		std::streamsize n = f.gcount();
		for (std::streamsize i = 0; i < n; i++) {
			h ^= (uint8_t)buf[i];
			h *= 0x100000001b3ULL;
		}
		if (!f)
			break;
	}

	return h;
}

enum action_code {
	ACT_CFG_LOAD = 0,
	ACT_CFG_SAVE = 1,
	ACT_INITIALIZED = 2,
	ACT_READY = 3,
	ACT_PERIODIC_SECOND = 4,
	ACT_ALARM_BEFORE = 6,
	ACT_ALARM_AFTER = 7,
	ACT_DEV_CHANGE = 8
};

/* Payload handed to Plugin::action().
   data is owned by the host and is only valid for the duration of the
   call - copy anything that has to outlive it. */
struct ActionEvent {
	/* what happened, one of the ACT_* codes */
	int code;
	/* action specific scalar, e.g. the bus number for the alarm
	   actions, 0 when the action carries no scalar */
	int val;
	/* optional structured payload (device rom, changed pio, ...),
	   nullptr when the action carries no data */
	const json* data;
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
	/* called on various events, ev.code is one of:
		0 - config load
		1 - config save
		2 - initialized (after init and plugins_init)
		3 - devices ready for access
		4 - every second (for regular tasks)
		6 - on alarm (before default handling), ev.val = bus number
	     returns 0 for ok and no action
			< 0 for error
			> 0 for more action needed (e.g. updating)
		7 - on alarm (after default handling), ev.val = bus number
		 returns:
			< 0 - error, no further action, abort
			0 - ok, nothing done and continue processing alarms (default)
			1 - Handled, continue processing alarms
			2 - Handled and no more action allowed, stop processing alarms
	*/
	virtual int action(const ActionEvent& ev) = 0;

	// config data set and get...
	virtual json config_get() const = 0;
	virtual void config_set(const json& j) = 0;
};

// Define the signature of the factory function
typedef Plugin* (*create_t)();
typedef void (*destroy_t)(Plugin*);
