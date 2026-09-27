#include <cstring>
#include <string>
#include <cerrno>
#include <unistd.h>
#include <fuse3/fuse.h>
#include <string>
#include <cctype>
#include "main.h"
#include "fs.h"
#include "fs_table.h"
#include "fs_leaf.h"
#include "ow_devices.h"
#include "plugins.h"
#include "switch_handler.h"
#include "ard_i2c.h"

using std::string;
extern Plugins plugins;
class OwDevices;
extern OwDevices ow;
class SwitchHandler;
extern SwitchHandler swHdl;

static struct fuse* g_fuse = nullptr;

static struct filetype root_dir[] = {
	{ ".", 0 },
	{ "..", 0 },
	{ "alarm", 0 },
	{ "uncached", 0 },
	{ "switches", 0 },
};

static struct filetype settings[] = {
	{ ".", 0 },
	{ "..", 0 },
	/* a directory: one file per loaded plugin, see plugin_name() */
	{ "plugins", 0 },
};

/* Plugin management lives under here: the directory lists one regular
   file per loaded plugin, creating a file loads that plugin, and
   writing to one controls it (see fs_write). */
static const char plugin_dir[] = "/settings/plugins";

/* Returns the plugin name for a "/settings/plugins/<name>" path, or an
   empty string for anything else - including the directory itself and
   any attempt to nest further below a plugin. */
static string plugin_name(const char* path)
{
	size_t len = strlen(plugin_dir);

	if (strncmp(path, plugin_dir, len) != 0 || path[len] != '/')
		return string();
	string name(path + len + 1);
	if (name.empty() || name.find('/') != string::npos)
		return string();

	return name;
}

static void fs_dir_devs(fuse_fill_dir_t filler, void *buf, bool alarm = false);
static bool check_path(std::string &spath, const filetype &s, struct stat *st);

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
	if (busNumber < 0 || busNumber >= MAX_BUS)
		return false;
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
	// one file per loaded plugin. Checked against the raw path before
	// the settings block below, which rewrites spath as it matches.
	string pname = plugin_name(path);
	if (!pname.empty()) {
		json cfg = plugins.config_of(pname);
		if (cfg.is_null())
			// no such plugin loaded; fs_create() is what adds one
			return -ENOENT;
		st->st_mode = S_IFREG | 0666;
		st->st_nlink = 1;
		st->st_size = cfg.dump().size() + 1;
		return 0;
	}
	{
		int attr = fsLeaf.attr(spath);
		if (attr > 0) {
			st->st_mode = S_IFREG | 0666;
			st->st_size = attr;
			st->st_nlink = 1;
			return 0;
		}
		if (attr == 0) {
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 2;
			return 0;
		}
	}
	if (extract_subpath(spath, "settings")) {
		for (const auto& s : settings)
			if (check_path(spath, s, st))
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
		for (const auto& n : fsLeaf.dir(string("")))
			filler(buf, n.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, "/settings") == 0) {
		for (const auto& s : settings)
			filler(buf, s.name, nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		for (const auto& n : fsLeaf.dir(string("settings")))
			filler(buf, n.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, plugin_dir) == 0) {
		for (const auto& name : plugins.names())
			filler(buf, name.c_str(), nullptr, 0,
				static_cast<fuse_fill_dir_flags>(0));
		filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
		return 0;
	}
	if (strcmp(path, "/log") == 0) {
		for (const auto& n : fsLeaf.dir(string("log")))
			filler(buf, n.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
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

	if (fsLeaf.open(spath) == 0)
		return 0;
	// a loaded plugin's control file; explicit rather than relying on
	// the SwitchHandler fallback at the end to wave it through
	if (!plugin_name(path).empty())
		return 0;

	int bus;
	extractBusNumber(spath, bus);
	extract_subpath(spath, "uncached");
	string rom;
	bool ret = extractRom(spath, rom);
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
	{
		int res = fsLeaf.read(spath, buf, size, false);
		if (res != fs_table::NOT_FOUND)
			return res;
	}
	// reading a plugin file reports that plugin's config
	string pname = plugin_name(path);
	if (!pname.empty()) {
		json cfg = plugins.config_of(pname);
		if (cfg.is_null())
			return -ENOENT;
		string s = cfg.dump() + "\n";
		if (s.size() > size)
			return -EINVAL;
		memcpy(buf, s.c_str(), s.size());
		return s.size();
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
	string spath(path);
	spath.erase(0, 1);
	{
		int res = fsLeaf.write(spath, buf, size);
		if (res != fs_table::NOT_FOUND)
			return res;
	}
	// Commands written to a plugin file. "/settings/plugins" itself is a
	// directory now, so it cannot be the target of a write any more -
	// the kernel refuses to open a directory for writing.
	string pname = plugin_name(path);
	if (!pname.empty()) {
		string cmd(buf, size);
		// trim the newline a shell redirect adds
		while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
			cmd.pop_back();

		if (cmd == "rm")
			return plugins.remove(pname) == 0 ? (int)size : -ENOENT;
		if (cmd == "reload") {
			plugins.reload();
			plugins.action(ACT_READY, 0);
			return size;
		}
		logger.warn("plugins: unknown command '" + cmd + "', expected rm or reload");
		return -EINVAL;
	}
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

	return -ENOENT;
}

/* Creating a file under the plugins directory loads that plugin, which
   is what makes "touch /settings/plugins/<name>" the way to add one.
   Nothing else in this filesystem is creatable. */
static int fs_create(const char* path, mode_t, struct fuse_file_info*)
{
	string pname = plugin_name(path);

	if (pname.empty())
		return -EPERM;
	switch (plugins.add(pname)) {
		case 0:
			logger.info("plugins: loaded " + pname);
			return 0;
		case 1:
			// already loaded, treat as success so touch stays idempotent
			return 0;
		default:
			// no such library, or it failed to load
			return -ENOENT;
	}
}

/* Deleting a plugin file unloads that plugin, making
   "rm /settings/plugins/<name>" the counterpart of the touch above.
   Nothing else in this filesystem is removable. */
static int fs_unlink(const char* path)
{
	string pname = plugin_name(path);

	if (pname.empty())
		return -EPERM;
	if (plugins.remove(pname) != 0)
		return -ENOENT;
	logger.info("plugins: unloaded " + pname);

	return 0;
}

/* touch() sets the timestamps right after opening the file; without
   this it would fail on that step even though the plugin did load.
   Timestamps are not stored, so this only has to succeed.
 *
 * It is also how "touch" on an *existing* plugin reloads it: because
 * the file is already there the kernel resolves it and calls open() +
 * utimens(), never create(), so this is the only hook that sees it. */
static int fs_utimens(const char* path, const struct timespec[2], struct fuse_file_info*)
{
	string pname = plugin_name(path);

	if (!pname.empty()) {
		plugins.reload(pname);
		plugins.action(ACT_READY, 0);
	}

	return 0;
}

static void* init(struct fuse_conn_info*, struct fuse_config*)
{
	g_fuse = fuse_get_context()->fuse;
	return nullptr;
}

void fs_init(fuse_operations* fs_ops) {
	fs_ops->getattr = fs_getattr;
	fs_ops->readdir = fs_readdir;
	fs_ops->create	= fs_create;
	fs_ops->unlink	= fs_unlink;
	fs_ops->utimens = fs_utimens;
	fs_ops->open	= fs_open;
	fs_ops->read	= fs_read;
	fs_ops->write   = fs_write;
	fs_ops->init	= init;
}
