#pragma once
#include <string>
#include <vector>
#include "pin.h"

class IDev {
public:
	virtual ~IDev() {}
	/* copies: the device may be renamed from another thread at any time,
	   a pointer into it would not stay valid */
	virtual std::string get_type() const = 0;
	virtual std::string get_name() const = 0;
};

/* A pin that was given a role, see IDevices::pins(). A copy, taken under
   the device lock; name is what the pin is shown as, "PIO.<n>" while it
   has none. */
struct PinInfo : Pin {
	std::string rom;
	int pin = 0;
	/* configured as an output; only outputs are switched by role */
	bool output = false;
	/* an output: whether it is on as far as known; an input: its sensed
	   level, as sensed.<n> reports it */
	bool on = false;
};

class IDevices {
public:
	virtual ~IDevices() {};
    virtual IDev* get_dev(uint64_t targetCode) = 0;
	/* every pin with a role, or with PinRole::none for any role */
	virtual std::vector<PinInfo> pins(PinRole role = PinRole::none) = 0;
	/* Switches every output pin with that role on or off, leaving the
	   ones already there alone. Returns how many could not be
	   switched, 0 when all went fine. */
	virtual int role_switch(PinRole role, bool on) = 0;
	/* The astro flag of the switches (see SW_COND_DARK): true from
	   sunset to sunrise. With a room (a room name, see room_parse())
	   that room's own flag, which its pins' switches go by instead. */
	virtual void dark_set(bool dark, const std::string& room) = 0;
	/* the room follows the astro flag again */
	virtual void dark_clear(const std::string& room) = 0;
	/* the flag in effect, for a room its own or else the astro flag */
	virtual bool is_dark(const std::string& room) = 0;
};
