#ifndef _IPIN_H
#define _IPIN_H
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

// What is connected to a pin, as opposed to how the pin behaves (its
// func). Only known to the host, never written to a device, and used
// to find e.g. every light regardless of the device it hangs on.
enum class PinRole {
	none,
	// outputs
	light,
	socket,
	fan,
	heating,
	blind,
	// inputs
	button,
	contact,
	motion,
};

inline const char* pin_role_str(PinRole r)
{
	switch (r) {
		case PinRole::light: return "light";
		case PinRole::socket: return "socket";
		case PinRole::fan: return "fan";
		case PinRole::heating: return "heating";
		case PinRole::blind: return "blind";
		case PinRole::button: return "button";
		case PinRole::contact: return "contact";
		case PinRole::motion: return "motion";
		case PinRole::none: break;
	}
	return "none";
}

// case-insensitive, an empty string is PinRole::none; nothing for an
// unknown role
inline std::optional<PinRole> pin_role_parse(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return std::tolower(c); });
	if (s.empty())
		return PinRole::none;
	for (int i = (int)PinRole::none; i <= (int)PinRole::motion; i++) {
		if (s == pin_role_str((PinRole)i))
			return (PinRole)i;
	}
	return std::nullopt;
}

// What the host knows about a pin of a device, besides the device's own
// config and registers (which stay with the device: they are read and
// written a byte or a block at a time, for all pins together).
struct Pin {
	// user given name, empty means the default "PIO.<n>"
	std::string name;
	PinRole role = PinRole::none;
	std::string room;
	// PWM level in percent as last applied by the device's level_set();
	// not saved with the config
	uint8_t level = 0;
};

#endif
