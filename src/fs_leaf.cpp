#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include "main.h"
#include "fs_leaf.h"
#include "ow_devices.h"
#include "version.h"

extern OwDevices ow;

// out-of-line definition of the private static members declared in
// fs_leaf.h; this counts as class scope for access control, so it can
// take the address of the private handlers below directly
const FsEntry<FsLeaf> FsLeaf::table[] = {
	{ "settings/log", 1, 0, false, nullptr, nullptr, &FsLeaf::r_log, &FsLeaf::w_log },
	{ "settings/poll", 3, 0, false, nullptr, nullptr, &FsLeaf::r_poll, &FsLeaf::w_poll },
	// before "settings/save": rows match as substrings of the path
	{ "settings/save_interval", 5, 0, false, nullptr, nullptr, &FsLeaf::r_save_interval, &FsLeaf::w_save_interval },
	// write anything: saves the config now
	{ "settings/save", 1, 0, false, nullptr, nullptr, nullptr, &FsLeaf::w_save },
	{ "status/version", 0, 0, false, nullptr, &FsLeaf::version_size, &FsLeaf::r_version, nullptr },
	{ "status/health", 0, 0, false, nullptr, &FsLeaf::health_size, &FsLeaf::r_health, nullptr },
	// TODO query the real size: ow.log_size()
	{ "status/1wire.vcd", 218 + (1024 * 30), 0, false, nullptr, nullptr, &FsLeaf::r_1wire, nullptr },
};
const size_t FsLeaf::n_table = sizeof(FsLeaf::table) / sizeof(FsLeaf::table[0]);

FsLeaf fsLeaf;

// for status/health: when the daemon started
static const auto started = std::chrono::steady_clock::now();

int FsLeaf::r_log(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", (int)logger.get_level());
	return std::strlen(buf);
}

int FsLeaf::w_log(const char* buf, size_t size, int)
{
	try {
		int tmp = std::stoi(buf);
		if (tmp < 0 || tmp > 8)
			return -EINVAL;
		logger.set_level((LogLevel)tmp);
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int FsLeaf::r_poll(char* buf, size_t, bool, int)
{
	std::sprintf(buf, "%d", ow.get_poll());
	return std::strlen(buf);
}

int FsLeaf::w_poll(const char* buf, size_t size, int)
{
	try {
		uint8_t tmp = (uint8_t)(std::stoi(buf));
		ow.set_poll(tmp);
		return size;
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
}

int FsLeaf::w_save(const char*, size_t size, int)
{
	int r = ow.save();
	return r == 0 ? (int)size : r;
}

int FsLeaf::r_save_interval(char* buf, size_t size, bool, int)
{
	std::snprintf(buf, size, "%d", ow.get_save_interval());
	return std::strlen(buf);
}

// seconds, 0 switches the timer off, at most a day
int FsLeaf::w_save_interval(const char* buf, size_t size, int)
{
	std::string s(buf, size);
	size_t end = 0;
	long v;

	try {
		v = std::stol(s, &end);
	} catch (const std::invalid_argument&) {
		return -EINVAL;
	} catch (const std::out_of_range&) {
		return -EINVAL;
	}
	while (end < s.size() && (s[end] == '\n' || s[end] == '\r' || s[end] == '\0'))
		end++;
	if (end != s.size() || v < 0 || v > 24 * 3600)
		return -EINVAL;
	ow.set_save_interval((int)v);
	return size;
}

int FsLeaf::r_1wire(char* buf, size_t size, bool, int)
{
	return ow.log_dump(buf, size);
}

int FsLeaf::r_version(char* buf, size_t size, bool, int)
{
	std::snprintf(buf, size, "%s\n", version_string());
	return std::strlen(buf);
}

int FsLeaf::version_size(int) const
{
	return (int)std::strlen(version_string()) + 1;
}

/* "2026-10-09 08:12:03" in the local time zone, like the journal */
static std::string local_time(std::chrono::system_clock::time_point tp)
{
	auto t = std::chrono::floor<std::chrono::seconds>(tp);
	try {
		return std::format("{:%F %T}", std::chrono::zoned_time{std::chrono::current_zone(), t});
	} catch (const std::exception&) {
		// no time zone data: UTC, marked as such
		return std::format("{:%F %T} UTC", t);
	}
}

/* "<last time> <message>", "-" while there was none */
static std::string recent_line(const Logger::Recent& r)
{
	if (r.count == 0)
		return "-";
	return local_time(r.when) + " " + r.last;
}

/* uptime in seconds, warning and error counts since the start, the
   last of each:

   uptime 86400
   warnings 3
   errors 0
   last_warning 2026-10-09 08:12:03 29.0701F8FE6677F4: DS2450 read result CRC mismatch
   last_error -
   last_save 2026-10-09 08:00:00
*/
std::string FsLeaf::health_text() const
{
	auto up = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - started).count();
	Logger::Recent w = logger.warnings();
	Logger::Recent e = logger.errors();
	auto saved = ow.last_saved();
	std::string last_save = saved.time_since_epoch().count() == 0 ? std::string("-")
		: local_time(saved);

	return std::format("uptime {}\nwarnings {}\nerrors {}\nlast_warning {}\nlast_error {}\nlast_save {}\n",
		up, w.count, e.count, recent_line(w), recent_line(e), last_save);
}

int FsLeaf::r_health(char* buf, size_t size, bool, int)
{
	std::snprintf(buf, size, "%s", health_text().c_str());
	return std::strlen(buf);
}

int FsLeaf::health_size(int) const
{
	return (int)health_text().size();
}

std::vector<std::string> FsLeaf::dir(const std::string& path) const
{
	return fs_table::dir(*this, table, n_table, path);
}

int FsLeaf::attr(const std::string& path) const
{
	return fs_table::attr(*this, table, n_table, path);
}

int FsLeaf::open(const std::string& path) const
{
	return fs_table::open(table, n_table, path);
}

int FsLeaf::read(const std::string& path, char* buf, size_t size, bool uncached)
{
	return fs_table::read(*this, table, n_table, path, buf, size, uncached);
}

int FsLeaf::write(const std::string& path, const char* buf, size_t size)
{
	return fs_table::write(*this, table, n_table, path, buf, size);
}
