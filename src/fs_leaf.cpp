#include <cerrno>
#include <cstring>
#include "main.h"
#include "fs_leaf.h"
#include "ow_devices.h"

extern OwDevices ow;

// out-of-line definition of the private static members declared in
// fs_leaf.h; this counts as class scope for access control, so it can
// take the address of the private handlers below directly
const FsEntry<FsLeaf> FsLeaf::table[] = {
	{ "settings/log", 1, 0, false, nullptr, nullptr, &FsLeaf::r_log, &FsLeaf::w_log },
	{ "settings/poll", 3, 0, false, nullptr, nullptr, &FsLeaf::r_poll, &FsLeaf::w_poll },
	// TODO query the real size: ow.log_size()
	{ "log/1wire.vcd", 218 + (1024 * 30), 0, false, nullptr, nullptr, &FsLeaf::r_1wire, nullptr },
};
const size_t FsLeaf::n_table = sizeof(FsLeaf::table) / sizeof(FsLeaf::table[0]);

FsLeaf fsLeaf;

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

int FsLeaf::r_1wire(char* buf, size_t size, bool, int)
{
	return ow.log_dump(buf, size);
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
