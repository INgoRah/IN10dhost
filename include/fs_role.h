#ifndef _FS_ROLE_H
#define _FS_ROLE_H
#include <string>
#include <vector>
#include <sys/types.h>

// The pins by role across all devices, under /role:
//
//   role/                    one directory per role (see pin_role.h)
//   role/<role>/all          read: how many outputs with that role are
//                            on; write 0 or 1: switch them all
//   role/<role>/list         one line per pin: "<rom>.<pin>\ton|off\t
//                            <room>\t<name>"
//   role/<role>/<rom>.<pin>  read: 1 on, 0 off; write 0 or 1 to switch
//                            that one output
//
// Paths are given without the leading slash, like FsLeaf's. Every
// function returns fs_table::NOT_FOUND for a path outside of "role".
namespace fs_role {
	// > 0 a file of that size, 0 a directory, or -ENOENT
	int attr(const std::string& path);
	// fills names, returns 0 or -ENOENT
	int dir(const std::string& path, std::vector<std::string>& names);
	int read(const std::string& path, char* buf, size_t size, off_t offset);
	int write(const std::string& path, const char* buf, size_t size);
}
#endif
