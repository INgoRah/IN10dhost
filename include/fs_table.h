#ifndef _FS_TABLE_H
#define _FS_TABLE_H

#include <algorithm>
#include <climits>
#include "fs.h"

// Table-driven IFs helper.
//
// A device lists its files once, as a flat array of FsEntry<T>, and
// gets fs_dir()/fs_attr()/fs_read()/fs_write() for free by forwarding
// to the fs_table::* functions below - see ds2408.cpp for the full
// pattern.
//
// A name containing '*' describes a whole family of files at once,
// expanded for idx in [0, count): "PIO.*" with count=8 covers PIO.0 ..
// PIO.7. Set alpha instead to expand over letters: "volt.*" with
// count=4 and alpha=true covers volt.A .. volt.D, and every callback
// still receives idx as 0..3 - the letter is only how it is spelled on
// disk, "A" -> 0, "B" -> 1, and so on. One entry replaces what used to
// be a hand-written 0..N loop either way.
//
// A name can also contain a slash to describe a file one level under
// a directory that is never listed as its own entry: pin dot star
// slash name, and pin dot star slash func, together are enough to
// make pin.0, pin.1, ... show up as directories, each containing
// "name" and "func" - there is no separate "pin.*" row, the directory
// only exists because these two do. A path never reaches a
// read()/write() callback without a slash in it actually matching one
// of these all the way, so the "directory" itself never needs one.
// (Spelled out above instead of written with the literal characters,
// since a slash right after a star would close this very comment.)
template <class T>
struct FsEntry {
	const char *name;
	// advertised via getattr(); ignored when size_cb is set
	int suglen;
	// number of wildcard instances; 0 means "name" has no '*' in it
	int count;
	// false: '*' is idx as a decimal number (0, 1, 2, ...).
	// true: '*' is idx as a capital letter (A, B, C, ...).
	bool alpha;
	// nullptr = always listed. Otherwise called once per idx to decide
	// whether that instance appears in fs_dir(); fs_attr()/fs_read()/
	// fs_write() do not consult this - a path that is syntactically
	// valid always works, matching how getattr already behaved on a
	// device before this file existed.
	bool (T::*visible)(int idx) const;
	// optional dynamic size, called instead of using suglen directly
	int (T::*size_cb)(int idx) const;
	int (T::*read)(char *buf, size_t size, bool uncached, int idx);
	int (T::*write)(const char *buf, size_t size, int idx);
};

namespace fs_table {

// Returned by read()/write()/attr() when nothing in the table matches,
// so the caller knows to fall back to the base class (OwDev::fs_*).
// Chosen so it can never collide with a real byte count or -errno.
constexpr int NOT_FOUND = INT_MIN;

namespace detail {

	// name with '*' replaced by idx, as a decimal number or as a
	// capital letter (idx 0 -> 'A') depending on alpha
	inline std::string instance_name(const char *name, int idx, bool alpha)
	{
		std::string s(name);
		size_t pos = s.find('*');
		if (pos == std::string::npos)
			return s;
		s.replace(pos, 1, alpha ? std::string(1, (char)('A' + idx)) : std::to_string(idx));
		return s;
	}

	// Finds the entry (and, for a wildcard entry, the idx) that "path"
	// names exactly - i.e. whose whole instantiated name, '/' and all,
	// occurs in path. Substring match, same convention every
	// fs_attr()/fs_read() in this codebase already used; a wildcard
	// entry is tried for every idx in [0, count).
	template <class T>
	const FsEntry<T> *match(const FsEntry<T> *table, size_t n,
		const std::string &path, int &idx)
	{
		for (size_t i = 0; i < n; i++) {
			const FsEntry<T> &e = table[i];
			if (e.count == 0) {
				if (path.find(e.name) != std::string::npos) {
					idx = -1;
					return &e;
				}
				continue;
			}
			for (int j = 0; j < e.count; j++) {
				if (path.find(instance_name(e.name, j, e.alpha)) != std::string::npos) {
					idx = j;
					return &e;
				}
			}
		}
		return nullptr;
	}

	// True when "path" is exactly "prefix", found anywhere (the usual
	// convention in this file) but with nothing after it besides
	// maybe one trailing slash. This is what tells a real directory
	// query ("pin.0", or readdir's "pin.0/") apart from a path that
	// merely starts with the same text but continues with something
	// this table never claimed, such as "settings/mode" next to
	// entries "settings/log" and "settings/poll": both start with
	// "settings", but only a bare "settings" names that directory.
	inline bool names_dir(const std::string &path, const std::string &prefix)
	{
		size_t pos = path.find(prefix);
		if (pos == std::string::npos)
			return false;
		size_t end = pos + prefix.size();
		return end == path.size() || (end + 1 == path.size() && path[end] == '/');
	}

	// True when "path" names an *implied* directory: not any entry
	// itself, but a prefix that some entry's name has before its '/'.
	// "pin.*/name" implies that pin.0, pin.1, ... are directories,
	// without "pin.*" ever being its own entry.
	template <class T>
	bool implied_dir(const FsEntry<T> *table, size_t n, const std::string &path)
	{
		for (size_t i = 0; i < n; i++) {
			const FsEntry<T> &e = table[i];
			std::string name(e.name);
			size_t slash = name.find('/');
			if (slash == std::string::npos)
				continue;
			for (int j = 0; j < std::max(e.count, 1); j++) {
				int idx = e.count ? j : -1;
				std::string prefix = instance_name(name.substr(0, slash).c_str(), idx, e.alpha);
				if (names_dir(path, prefix))
					return true;
			}
		}
		return false;
	}

} // namespace detail

// True when "path" names a directory implied by some entry's name -
// see implied_dir(). dir() uses this to return that directory's
// contents instead of the device's top-level listing.
template <class T>
bool in_instance_dir(const FsEntry<T> *table, size_t n, const std::string &path)
{
	return detail::implied_dir(table, n, path);
}

// Directory listing: either the contents of the implied directory
// "path" is browsing into, or the device's own top-level file/dir
// names (each implied directory listed once, not once per file in it).
template <class T>
std::vector<std::string> dir(const T &self, const FsEntry<T> *table, size_t n,
	const std::string &path)
{
	std::vector<std::string> out;

	for (size_t i = 0; i < n; i++) {
		const FsEntry<T> &e = table[i];
		std::string name(e.name);
		size_t slash = name.find('/');
		if (slash == std::string::npos)
			continue;
		for (int j = 0; j < std::max(e.count, 1); j++) {
			int idx = e.count ? j : -1;
			std::string prefix = detail::instance_name(name.substr(0, slash).c_str(), idx, e.alpha);
			if (detail::names_dir(path, prefix))
				out.push_back(detail::instance_name(name.substr(slash + 1).c_str(), idx, e.alpha));
		}
	}
	if (!out.empty())
		return out;

	for (size_t i = 0; i < n; i++) {
		const FsEntry<T> &e = table[i];
		std::string name(e.name);
		size_t slash = name.find('/');
		for (int j = 0; j < std::max(e.count, 1); j++) {
			int idx = e.count ? j : -1;
			if (e.visible && !(self.*e.visible)(idx))
				continue;
			std::string inst = detail::instance_name(
				(slash == std::string::npos ? name : name.substr(0, slash)).c_str(), idx, e.alpha);
			if (std::find(out.begin(), out.end(), inst) == out.end())
				out.push_back(inst);
		}
	}
	return out;
}

// getattr(): 0 for an implied directory, else suglen or size_cb(idx).
// Matches unconditionally - see the "visible" comment on FsEntry.
template <class T>
int attr(const T &self, const FsEntry<T> *table, size_t n, const std::string &path)
{
	int idx;
	const FsEntry<T> *e = detail::match(table, n, path, idx);

	if (e)
		return e->size_cb ? (self.*e->size_cb)(idx) : e->suglen;
	if (detail::implied_dir(table, n, path))
		return 0;
	return NOT_FOUND;
}

// open(): 0 for an entry with a read handler, NOT_FOUND otherwise (be
// it no match at all, or a write-only entry such as a command file).
// Most IFs implementers do not need this - OwDev's own fs_open() just
// unconditionally succeeds - so use it only where individual entries
// genuinely differ in whether they can be opened.
template <class T>
int open(const FsEntry<T> *table, size_t n, const std::string &path)
{
	int idx;
	const FsEntry<T> *e = detail::match(table, n, path, idx);

	if (e && e->read)
		return 0;
	return NOT_FOUND;
}

template <class T>
int read(T &self, const FsEntry<T> *table, size_t n, const std::string &path,
	char *buf, size_t size, bool uncached)
{
	int idx;
	const FsEntry<T> *e = detail::match(table, n, path, idx);

	if (!e || !e->read)
		return NOT_FOUND;
	return (self.*e->read)(buf, size, uncached, idx);
}

template <class T>
int write(T &self, const FsEntry<T> *table, size_t n, const std::string &path,
	const char *buf, size_t size)
{
	int idx;
	const FsEntry<T> *e = detail::match(table, n, path, idx);

	if (!e || !e->write)
		return NOT_FOUND;
	return (self.*e->write)(buf, size, idx);
}

} // namespace fs_table
#endif /* _FS_TABLE_H */
