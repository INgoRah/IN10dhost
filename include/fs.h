#pragma once
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
