#pragma once
#include <fuse3/fuse.h>

using std::string;

struct filetype {
	const char *name;
	int suglen;					// length of field
};

class IFs {
	public:
		virtual ~IFs() {};
		virtual std::vector<string> fs_dir(string& path) const = 0;
		virtual int fs_read(string& path, char* buf, size_t size, bool uncached) = 0;
		virtual int fs_attr(string& path) const = 0;
		virtual int fs_open(string& path) const = 0;
		virtual int fs_write(string& path, const char* buf, size_t size) = 0;
};

void fs_init(fuse_operations* fs_ops);

extern struct fuse_operations simple_fs_ops;
