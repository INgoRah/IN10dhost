#include <cerrno>
#include <cstring>
#include <format>
#include "main.h"
#include "fs.h"
#include "fs_table.h"
#include "fs_room.h"
#include "ow_devices.h"

extern OwDevices ow;

namespace {

enum Level { L_ROOT, L_ROOM, L_FILE };

// Splits "rooms[/<room>[/<file>]]", the room lower-cased. Returns the
// level, NOT_FOUND outside of "rooms" or -ENOENT for no room name, an
// unknown file or anything deeper.
int split(std::string path, std::string& room, std::string& file)
{
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	if (path == "rooms")
		return L_ROOT;
	if (path.rfind("rooms/", 0) != 0)
		return fs_table::NOT_FOUND;
	path.erase(0, 6);
	size_t slash = path.find('/');
	auto r = room_parse(path.substr(0, slash));
	if (!r || r->empty())
		return -ENOENT;
	room = *r;
	if (slash == std::string::npos)
		return L_ROOM;
	file = path.substr(slash + 1);
	if (file != "dark" && file != "list")
		return -ENOENT;
	return L_FILE;
}

std::string list_text(const std::vector<PinInfo>& pins)
{
	std::string s;

	for (const auto& p : pins)
		s += std::format("{}.{}\t{}\t{}\t{}\n", p.rom, p.pin, pin_role_str(p.role),
			p.on ? "on" : "off", p.name);
	return s;
}

std::string content(const std::string& room, const std::string& file)
{
	if (file == "dark")
		return ow.is_dark(room) ? "1\n" : "0\n";
	return list_text(ow.room_pins(room));
}

} // namespace

namespace fs_room {

int attr(const std::string& path)
{
	std::string room, file;
	int level = split(path, room, file);

	if (level == L_ROOT || level == L_ROOM)
		return 0;
	if (level != L_FILE)
		return level;
	std::string s = content(room, file);
	// an empty list is still a file
	return s.empty() ? 1 : (int)s.size();
}

int dir(const std::string& path, std::vector<std::string>& names)
{
	std::string room, file;
	int level = split(path, room, file);

	if (level == L_ROOT) {
		names = ow.rooms();
	} else if (level == L_ROOM) {
		names.push_back("dark");
		names.push_back("list");
	} else {
		return level == L_FILE ? -ENOTDIR : level;
	}
	names.push_back(".");
	names.push_back("..");
	return 0;
}

int read(const std::string& path, char* buf, size_t size, off_t offset)
{
	std::string room, file;
	int level = split(path, room, file);

	if (level == L_ROOT || level == L_ROOM)
		return -EISDIR;
	if (level != L_FILE)
		return level;
	std::string s = content(room, file);
	// the list can outgrow one read
	if (offset < 0 || (size_t)offset >= s.size())
		return 0;
	size_t n = std::min(size, s.size() - (size_t)offset);
	memcpy(buf, s.data() + offset, n);
	return (int)n;
}

int write(const std::string& path, const char* buf, size_t size)
{
	std::string room, file;
	int level = split(path, room, file);

	if (level == L_ROOT || level == L_ROOM)
		return -EISDIR;
	if (level != L_FILE)
		return level;
	if (file == "list")
		return -EPERM;
	std::string s(buf, size);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0'))
		s.pop_back();
	if (s == "0" || s == "1")
		ow.dark_set(s == "1", room);
	else if (s.empty() || s == "auto")
		ow.dark_clear(room);
	else
		return -EINVAL;
	return size;
}

} // namespace fs_room
