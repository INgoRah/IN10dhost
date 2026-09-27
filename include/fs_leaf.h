#ifndef _FS_LEAF_H
#define _FS_LEAF_H
#include <string>
#include <vector>
#include "fs_table.h"

// The leaf files with real behaviour under /settings and /log, one
// flat table keyed by their full path so each entry's '/' marks its
// own real directory. "plugins" stays in fs.cpp's settings[] array -
// it is a dynamic subtree fs_table.h has no way to describe.
class FsLeaf {
	private:
		// fs_table.h handlers, bound to the entries of the private
		// `table` member below (defined out-of-line in fs_leaf.cpp).
		// Not meant to be called directly.
		int r_log(char* buf, size_t size, bool uncached, int idx);
		int w_log(const char* buf, size_t size, int idx);
		int r_poll(char* buf, size_t size, bool uncached, int idx);
		int w_poll(const char* buf, size_t size, int idx);
		int r_1wire(char* buf, size_t size, bool uncached, int idx);
		static const FsEntry<FsLeaf> table[];
		static const size_t n_table;
	public:
		std::vector<std::string> dir(const std::string& path) const;
		int attr(const std::string& path) const;
		int open(const std::string& path) const;
		int read(const std::string& path, char* buf, size_t size, bool uncached);
		int write(const std::string& path, const char* buf, size_t size);
};

extern FsLeaf fsLeaf;
#endif
