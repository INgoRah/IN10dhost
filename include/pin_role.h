#ifndef _PIN_ROLE_H
#define _PIN_ROLE_H
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

#endif
