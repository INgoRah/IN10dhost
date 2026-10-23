#include <cerrno>
#include <cstring>
#include <format>
#include "main.h"
#include "fs.h"
#include "fs_table.h"
#include "fs_role.h"
#include "ow_devices.h"
#include "ds2408.h"

extern OwDevices ow;

namespace {

enum Level { L_ROOT, L_ROLE, L_FILE };

// Splits "role[/<role>[/<file>]]". Returns the level, NOT_FOUND outside
// of "role" or -ENOENT for an unknown role or anything deeper.
int split(std::string path, PinRole& role, std::string& file)
{
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	if (path == "role")
		return L_ROOT;
	if (path.rfind("role/", 0) != 0)
		return fs_table::NOT_FOUND;
	path.erase(0, 5);
	size_t slash = path.find('/');
	std::string name = path.substr(0, slash);
	// "none" is every pin without a role, not worth a directory
	auto r = pin_role_parse(name);
	if (!r || *r == PinRole::none || name != pin_role_str(*r))
		return -ENOENT;
	role = *r;
	if (slash == std::string::npos)
		return L_ROLE;
	file = path.substr(slash + 1);
	if (file.empty() || file.find('/') != std::string::npos)
		return -ENOENT;
	return L_FILE;
}

std::string pin_file(const PinInfo& p)
{
	return std::format("{}.{}", p.rom, p.pin);
}

std::string list_text(const std::vector<PinInfo>& pins)
{
	std::string s;

	for (const auto& p : pins)
		s += std::format("{}\t{}\t{}\t{}\n", pin_file(p), p.on ? "on" : "off", p.room, p.name);
	return s;
}

std::string all_text(const std::vector<PinInfo>& pins)
{
	int on = 0;

	for (const auto& p : pins)
		if (p.output && p.on)
			on++;
	return std::to_string(on);
}

const PinInfo* find_pin(const std::vector<PinInfo>& pins, const std::string& file)
{
	for (const auto& p : pins)
		if (pin_file(p) == file)
			return &p;
	return nullptr;
}

// What a file under role/<role>/ reads, false when there is no such file
bool content(PinRole role, const std::string& file, std::string& s)
{
	std::vector<PinInfo> pins = ow.pins(role);

	if (file == "all") {
		s = all_text(pins);
		return true;
	}
	if (file == "list") {
		s = list_text(pins);
		return true;
	}
	const PinInfo* p = find_pin(pins, file);
	if (!p)
		return false;
	s = p->on ? "1" : "0";
	return true;
}

// "0" or "1", a trailing newline or NUL allowed
bool parse_on(const char* buf, size_t size, bool& on)
{
	std::string s(buf, size);

	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0'))
		s.pop_back();
	if (s != "0" && s != "1")
		return false;
	on = s == "1";
	return true;
}

} // namespace

namespace fs_role {

int attr(const std::string& path)
{
	PinRole role;
	std::string file, s;
	int level = split(path, role, file);

	if (level == L_ROOT || level == L_ROLE)
		return 0;
	if (level != L_FILE)
		return level;
	if (!content(role, file, s))
		return -ENOENT;
	// an empty list is still a file
	return s.empty() ? 1 : (int)s.size();
}

int dir(const std::string& path, std::vector<std::string>& names)
{
	PinRole role;
	std::string file;
	int level = split(path, role, file);

	if (level == L_ROOT) {
		for (int i = (int)PinRole::none + 1; i <= (int)PinRole::motion; i++)
			names.push_back(pin_role_str((PinRole)i));
	} else if (level == L_ROLE) {
		names.push_back("all");
		names.push_back("list");
		for (const auto& p : ow.pins(role))
			names.push_back(pin_file(p));
	} else {
		return level == L_FILE ? -ENOTDIR : level;
	}
	names.push_back(".");
	names.push_back("..");
	return 0;
}

int read(const std::string& path, char* buf, size_t size, off_t offset)
{
	PinRole role;
	std::string file, s;
	int level = split(path, role, file);

	if (level == L_ROOT || level == L_ROLE)
		return -EISDIR;
	if (level != L_FILE)
		return level;
	if (!content(role, file, s))
		return -ENOENT;
	// the list can outgrow one read
	if (offset < 0 || (size_t)offset >= s.size())
		return 0;
	size_t n = std::min(size, s.size() - (size_t)offset);
	memcpy(buf, s.data() + offset, n);
	return (int)n;
}

int write(const std::string& path, const char* buf, size_t size)
{
	PinRole role;
	std::string file;
	int level = split(path, role, file);
	bool on;

	if (level == L_ROOT || level == L_ROLE)
		return -EISDIR;
	if (level != L_FILE)
		return level;
	if (file == "list")
		return -EPERM;
	if (file == "all") {
		if (!parse_on(buf, size, on))
			return -EINVAL;
		return ow.role_switch(role, on) == 0 ? (int)size : -EIO;
	}
	std::vector<PinInfo> pins = ow.pins(role);
	const PinInfo* p = find_pin(pins, file);
	if (!p)
		return -ENOENT;
	if (!p->output)
		return -EPERM;
	if (!parse_on(buf, size, on))
		return -EINVAL;
	auto* dev = dynamic_cast<ds2408*>(ow.find(p->rom));
	if (!dev || dev->stale())
		return -ENOENT;
	// coverity[sleep] - bus mutex must be held
	if (dev->pin_switch(p->pin, on ? ON : OFF) != 0)
		return -EIO;
	return size;
}

} // namespace fs_role
