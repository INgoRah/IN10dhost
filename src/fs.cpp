#include <cstring>
#include <string>
#include <cerrno>
#include <unistd.h>
#include <fuse3/fuse.h>
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "plugins.h"
#include "switch_handler.h"
#include "ard_i2c.h"

using std::string;
extern Plugins plugins;

static struct filetype root_dir[] = {
	{ ".", 0 },
	{ "..", 0 },
	{ "alarm", 0 },
	{ "uncached", 0 },
	{ "settings", 0 },
	{ "switches", 0 },
	{ "log", 0 },
};

static struct filetype settings[] = {
	{ ".", 0 },
	{ "..", 0 },
	{ "log", 1 },
	{ "mode", 3 },
	{ "poll", 3 },
	{ "plugins", 1024 },
};

static void fs_dir_devs(fuse_fill_dir_t filler, void *buf, bool alarm = false);

static bool check_path(std::string &spath, const filetype &s, struct stat *st);

static string file_content = "Hello from FUSE!\n";
static string f_buf = "\n";
static struct fuse* g_fuse = nullptr;

class OwDevices;
extern OwDevices ow;
class SwitchHandler;
extern SwitchHandler swHdl;

#include <string>
#include <cctype>

static bool extractBusNumber(string& path, int& busNumber)
{
	const string tag = "bus.";
	auto pos = path.find(tag);
	if (pos == string::npos)
		return false;

	size_t numStart = pos + tag.size();
	size_t numEnd = numStart;

	while (numEnd < path.size() && std::isdigit(path[numEnd]))
		++numEnd;

	if (numStart == numEnd)
		return false; // no number found

	busNumber = std::stoi(path.substr(numStart, numEnd - numStart));

	// erase "/bus.<number>/"
	path.erase(pos, numEnd - pos + 1);

	return true;
}

static bool extractRom(string& path, string& rom)
{
	auto pos = path.find("/");

	if (pos == 0) {
		// string of first /
		path.erase(pos, 1);
		// and search the next
		pos = path.find("/");
	}
	if (pos != string::npos)
		// string next /
		path.erase(pos, 1);
	pos = path.find(".");
	// now the rom must be between 0..pos and
	// must be 17 bytes 12.45 67 89 01 23 45 67
	if (pos == 2 && path.length() >= 17) {
		rom = path.substr(0, 17);
		if (path.length() > 17)
			path = path.erase(0, 17);
		//printf("%s %s %ld %ld\n", path.c_str(), rom.c_str(), pos, path.length());
		return true;
	}

	return false;
}

/* extract
*/
static bool extract_subpath(string& path, const char* subpart)
{
	auto pos = path.find(subpart);
	if (pos == string::npos)
		return false;
	pos = path.find("/");
	if (pos == 0) {
		// string of first /
		path.erase(pos, 1);
		// and search the next
		pos = path.find("/");
	}
	if (pos != string::npos)
		// string next /
		path.erase(pos, 1);
	pos = path.find(subpart);
	if (pos != string::npos) {
		// string next /
		path.erase(pos, strlen(subpart));
		return true;
	}

	return false;
}

static int fs_attr_rom(int bus, string spath, struct stat* st)
{
	(void)bus;
	const OwDev* dev = ow.find(spath);
	if (dev) {
		int attr = dev->fs_attr(spath);
		//printf("fs_getattr bus.%d.dev %s %d\n", bus, spath.c_str(), attr);
		if (attr > 0) {
			st->st_mode = S_IFREG | 0666;
			st->st_size = attr;
			st->st_nlink = 1;
			return 0;
		}
		if (attr == 0) {
			st->st_mode = S_IFDIR | 0777;
			st->st_nlink = 2;
			st->st_size = 0;
			return 0;
		}
	}
	return -ENOENT;
}

bool check_path(std::string &spath, const filetype &s, struct stat *st)
{
	if (spath == s.name)
	{
		if (s.suglen == 0)
		{
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 2;
			return true;
		}
		else
		{
			st->st_mode = S_IFREG | 0666;
			st->st_nlink = 1;
			st->st_size = s.suglen;
			return true;
		}
	}

	return false;
}

static int fs_getattr(const char* path, struct stat* st, struct fuse_file_info*)
{
	string spath(path);
	spath.erase(0, 1);
	memset(st, 0, sizeof(struct stat));

	// Directories
	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	for (const auto& s : root_dir) {
		if (check_path(spath, s, st))
			return 0;
	}
	if (extract_subpath(spath, "settings")) {
		for (const auto& s : settings)
			if (check_path(spath, s, st))
				return 0;
	}
	if (strcmp(path, "/log") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, "/log/1wire.vcd") == 0) {
		st->st_mode = S_IFREG | 0666;
		// TODO query real size ow.log_size()
		st->st_size = 218 + (1024 * 30);
		st->st_nlink = 1;
		return 0;
	}

	int bus;
	if (extractBusNumber(spath, bus)) {
		if (spath.length() > 0) {
			// below bus
			return fs_attr_rom(bus, spath, st);
		} else {
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 0;
			st->st_size = 0;
			return 0;
		}
	}
	extract_subpath(spath, "uncached");
	// if alarm list only devices in alarm state
	// guess all ROMs
	for (int bus = 0; bus < ow.bus_count(); bus++)
		if (fs_attr_rom(bus, spath, st) == 0)
			return 0;
	int res = swHdl.fs_attr(spath);
	if (res > 0) {
		st->st_mode = S_IFREG | 0666;
		st->st_size = res;
		st->st_nlink = 1;
		return 0;
	}
	if (res == 0) {
		st->st_mode = S_IFDIR | 0777;
		st->st_nlink = 2;
		st->st_size = 0;
		return 0;
	}

	return -ENOENT;
}

static void fs_dir_devs(fuse_fill_dir_t filler, void *buf, bool alarm)
{
	for (int bus = 0; bus < ow.bus_count(); bus++)
	{
		std::vector<OwDev *> devs = ow.list_devices(bus);
		for (const auto &dev : devs) {
			if (alarm && !dev->alarm)
				continue;
			filler(buf, dev->rom.c_str(), nullptr, 0,
				   static_cast<fuse_fill_dir_flags>(0));
		}
	}
}

static int fs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
					  off_t, struct fuse_file_info*, enum fuse_readdir_flags)
{
	bool ret;
	string spath(path);
	spath.erase(0, 1);

	ret = extract_subpath(spath, "uncached");
	if (ret) {
		if (spath.length() == 0) {
			logger.info("scanning ...");
			ow.search(true);
			fs_dir_devs(filler, buf, false);
		}
	}
	ret = extract_subpath(spath, "alarm");
	if (ret) {
		logger.info("alarm search ... ");
		ow.search(false);
		fs_dir_devs(filler, buf, true);
		return 0;
	}

	if (strcmp(path, "/") == 0) {
		for (int i = 0; i < ow.bus_count(); i++) {
			filler(buf, ("bus." + std::to_string(i)).c_str(), nullptr, 0,
				   static_cast<fuse_fill_dir_flags>(0));
		}
		fs_dir_devs(filler, buf, false);
		// standard entries
		for (const auto& s : root_dir)
			filler(buf, s.name, nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, "/settings") == 0) {
		for (const auto& s : settings)
			filler(buf, s.name, nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, "/log") == 0) {
		filler(buf, "1wire.vcd", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, "/switches") == 0) {
		std::vector<string> ls = swHdl.fs_dir(spath);

		for (const auto& s : ls) {
			filler(buf, s.c_str(), nullptr, 0,
				static_cast<fuse_fill_dir_flags>(0));
		}
		filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	int bus;
	ret = extractBusNumber(spath, bus);
	if (ret) {
		std::vector<OwDev*> devs = ow.list_devices(bus);
		for (const auto& dev : devs) {
			if (spath.length() > 0) {
			} else {
				// bus level
				// add device dir
				filler(buf, dev->rom.c_str(), nullptr, 0,
					static_cast<fuse_fill_dir_flags>(0));
			}
		}
	}
	if (spath.length() > 0) {
		// device level
		string rom;
		if (!extractRom(spath, rom))
			return -ENOENT;
		OwDev* dev = ow.find(rom);
		if (!dev)
			return -ENOENT;
		std::vector<string> ls = dev->fs_dir(spath);

		for (const auto& s : ls) {
			filler(buf, s.c_str(), nullptr, 0,
				static_cast<fuse_fill_dir_flags>(0));
		}
	}

	return 0;
	//return -ENOENT;
}

static int fs_open(const char* path, struct fuse_file_info*)
{
	string spath(path);
	spath.erase(0, 1);

	if (strcmp(path, "/settings/log") == 0)
		return 0;
	if (strcmp(path, "/settings/mode") == 0)
		return 0;
	if (strcmp(path, "/settings/poll") == 0)
		return 0;
	if (strcmp(path, "/log/1wire.vcd") == 0)
		return 0;

	int bus;
	bool ret = extractBusNumber(spath, bus);
	extract_subpath(spath, "uncached");
	string rom;
	ret = extractRom(spath, rom);
	if (ret) {
		const OwDev* dev = ow.find(rom);
		if (dev) {
			// check if open works dev->fs_open(spath)
			return dev->fs_open(spath);
		}
	}
	if (swHdl.fs_open(spath) == 0)
		return 0;
	return -ENOENT;
}

static int fs_read(const char* path, char* buf, size_t size, off_t offset,
				   struct fuse_file_info*)
{
	string spath(path);
	spath.erase(0, 1);
	(void)offset;
	if (strcmp(path, "/settings/log") == 0) {
		std::sprintf(buf, "%d", (int)logger.get_level());
			return std::strlen(buf);
	}
	if (strcmp(path, "/settings/mode") == 0) {
		std::sprintf(buf, "%d", ow.get_mode());
		return std::strlen(buf);
	}
	if (strcmp(path, "/settings/poll") == 0) {
		std::sprintf(buf, "%d", ow.get_poll());
		return std::strlen(buf);
	}
	if (strcmp(path, "/log/1wire.vcd") == 0) {
		return ow.log_dump(buf, size);
	}
	if (extract_subpath(spath, "switches"))
		return swHdl.fs_read(spath, buf, size);
	int bus;
	string rom;
	bool uncached = false;
	if (extract_subpath(spath, "uncached")) {
		uncached = true;
	}
	extractBusNumber(spath, bus);
	bool ret = extractRom(spath, rom);
	if (ret && spath.length() > 0) {
		OwDev* dev = ow.find(rom);
		if (dev)
			return dev->fs_read(spath, buf, size, uncached);
	}

	return -ENOENT;
}


static int fs_write(const char* path, const char* buf, size_t size,
					off_t offset, struct fuse_file_info*)
{
	(void)offset;
	if (strcmp(path, "/settings/log") == 0) {
		uint8_t tmp = (uint8_t)(std::stoi(buf));
		if (tmp > 8)
			tmp = 8;
		ow.set_log(tmp);
		logger.set_level((LogLevel)tmp);
		return size;
	}
	if (strcmp(path, "/settings/mode") == 0) {
		uint8_t tmp = (uint8_t)(std::stoi(buf));
		ow.set_mode(tmp);
		return size;
	}
	if (strcmp(path, "/settings/poll") == 0) {
		uint8_t tmp = (uint8_t)(std::stoi(buf));
		ow.set_poll(tmp);
		return size;
	}
	if (strcmp(path, "/settings/plugins") == 0) {
		plugins.reload();
		plugins.action(INITIALIZED, 0); // initialized
	}
	string spath(path);
	spath.erase(0, 1);
	int bus;
	extractBusNumber(spath, bus);
	string rom;
	int ret = extractRom(spath, rom);
	if (ret) {
		OwDev* dev = ow.find(rom);
		if (dev) {
			ret = dev->fs_write(spath, buf, size);
			return ret;
		}
	}
	if (extract_subpath(spath, "switches"))
		return swHdl.fs_write(spath, buf, size);

	return size;
}

static void* init(struct fuse_conn_info*, struct fuse_config*)
{
	g_fuse = fuse_get_context()->fuse;
	return nullptr;
}

void fs_init(fuse_operations* fs_ops) {
	fs_ops->getattr = fs_getattr;
	fs_ops->readdir = fs_readdir;
	fs_ops->open	= fs_open;
	fs_ops->read	= fs_read;
	fs_ops->write   = fs_write;
	fs_ops->init	= init;
}
