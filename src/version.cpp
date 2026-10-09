#include <string>
#include "version.h"
// generated into the build directory, see cmake/git_version.cmake
#include "git_version.h"

const char* version_string()
{
	static const std::string v = std::string(IN10DFS_VERSION) +
		(IN10DFS_GIT_HASH[0] ? std::string(" (") + IN10DFS_GIT_HASH + ")" : std::string());

	return v.c_str();
}
