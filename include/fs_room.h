#ifndef _FS_ROOM_H
#define _FS_ROOM_H
#include <string>
#include <vector>
#include <sys/types.h>

// The rooms of the pins (pin.<n>/room) across all devices, under /rooms:
//
//   rooms/                   one directory per room a pin is in or that
//                            has a dark flag of its own
//   rooms/<room>/dark        read: 1 dark, 0 day, the flag in effect for
//                            the room's switches; write 0 or 1: the
//                            room's own flag, "auto" or an empty write:
//                            the room follows /switches/dark again
//   rooms/<room>/list        one line per pin: "<rom>.<pin>\t<role>\t
//                            on|off\t<name>"
//
// Room names are lower case (see room_parse()), a path is matched
// case-insensitively: rooms/Kitchen is rooms/kitchen. Any room name
// can be looked up, so a flag can be set for a room no pin is in yet;
// only the known ones are listed.
//
// Paths are given without the leading slash, like FsLeaf's. Every
// function returns fs_table::NOT_FOUND for a path outside of "rooms".
namespace fs_room {
	// > 0 a file of that size, 0 a directory, or -ENOENT
	int attr(const std::string& path);
	// fills names, returns 0 or -ENOENT
	int dir(const std::string& path, std::vector<std::string>& names);
	int read(const std::string& path, char* buf, size_t size, off_t offset);
	int write(const std::string& path, const char* buf, size_t size);
}
#endif
