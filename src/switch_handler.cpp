#include <fstream>
#include <iostream>
#include <string>
// swtable parsing
#include <charconv>
#include <string_view>
#include <vector>
#include "nlohmann/json.hpp"

#include "main.h"
#include "fs.h"
#include "switch_handler.h"
#include "ow_devices.h"
#include "ds2408.h"
#include "ds1820.h"

struct _sw_tbl sw_tbl[MAX_SWITCHES];

// out-of-line definition of the private static members declared in
// switch_handler.h; this counts as class scope for access control, so
// it can take the address of the private handlers below directly.
// "add"/"del" are write-only (no read handler); fs_table::open() still
// opens them, since they otherwise could never be written to at all.
const FsEntry<SwitchHandler> SwitchHandler::table[] = {
	{ "add", 32, 0, false, nullptr, nullptr, nullptr, &SwitchHandler::w_add },
	{ "del", 32, 0, false, nullptr, nullptr, nullptr, &SwitchHandler::w_del },
	{ "list", 0, 0, false, nullptr, &SwitchHandler::list_size, &SwitchHandler::r_list, nullptr },
};
const size_t SwitchHandler::n_table = sizeof(SwitchHandler::table) / sizeof(SwitchHandler::table[0]);

void to_json(json& j, const _sw_tbl& b) {
	j = json{
		{"src_bus", b.src.sa.bus },
		{"src_adr", b.src.sa.adr },
		{"src_latch", b.src.sa.latch },
		{"src_press", b.src.sa.press },
		{"dst_bus", b.dst.da.bus},
		{"dst_adr", b.dst.da.adr},
		{"dst_pio", b.dst.da.pio},
		{"dst_type", b.dst.da.type},
		{"secs", b.secs},
		{"on_press", b.on_press}
	};
}

void from_json(const json& j, _sw_tbl& b) {
	int d;

	if (j.contains("src_adr")) {
		j.at("src_bus").get_to<int>(d);
		b.src.sa.bus = d;
		j.at("src_adr").get_to(d);
		b.src.sa.adr = d;
		j.at("src_latch").get_to(d);
		b.src.sa.latch = d;
		j.at("src_press").get_to(d);
		b.src.sa.press = d;
		j.at("dst_bus").get_to(d);
		b.dst.da.bus = d;
		j.at("dst_adr").get_to(d);
		b.dst.da.adr = d;
		j.at("dst_pio").get_to(d);
		b.dst.da.pio = d;
	}
	if (j.contains("dst_type")) {
		j.at("dst_type").get_to(d);
		b.dst.da.type = d;
	}
	// older configs have no timed switches
	if (j.contains("secs"))
		j.at("secs").get_to(b.secs);
	if (j.contains("on_press"))
		j.at("on_press").get_to(b.on_press);
}

bool parse_buf(std::string_view buf, struct _sw_tbl& sw)
{
	int vals[6];
	const char* ptr = buf.data();
	const char* end = buf.data() + buf.size();

	// zero first: src.sa.res and dst.da.type are never assigned below,
	// so without this sw.src.data/sw.dst.data (used for equality
	// elsewhere, e.g. deleting a switch) would compare whatever
	// garbage those bits held on the caller's stack instead of being
	// deterministic
	sw = {};
	if (buf.size() < (5 + 5)) {
		logger.warn (std::format("size mismatch {}", buf));
		return false;
	}
	// expecting any of
	// [bus] [adr] [latch] [bus] [adr] [pio]
	// [bus].[adr].[latch] [bus].[adr].[pio]
	// [bus].[adr].[latch] -> [bus].[adr].[pio]
	// '.', '-' and '>' are just delimiters here, same as whitespace -
	// from_chars already stops an int at the first one of these on its
	// own (a leading '-' it would otherwise read as a sign never makes
	// sense for these always-non-negative fields), so accepting all of
	// the above only takes skipping them between numbers as well.
	// Anything left over past the 6th number (including a stray
	// trailing "->") is simply never looked at below.
	for (int i = 0; i < 6; ++i) {
		// Skip leading whitespace/delimiter characters manually if needed
		while (ptr < end && (std::isspace(*ptr) || *ptr == '.' || *ptr == '-' || *ptr == '>')) ptr++;

		auto [next, ec] = std::from_chars(ptr, end, vals[i]);
		if (ec != std::errc{}) {
			logger.warn (std::format("parse error {}", buf));
			return false; // Parse error
		}
		ptr = next;
	}

	// Assign to your variables
	sw.src.sa.bus = (uint8_t)(vals[0] & 0xff);
	sw.src.sa.adr = (uint8_t)(vals[1] & 0xff);
	uint8_t latch = (uint8_t)(vals[2] & 0xff);
	if (latch - 20 > 0) {
		/* pressing */
		sw.src.sa.press = 2;
		sw.src.sa.latch = latch - 20;
	} else if (latch - 10 > 0) {
		/* press long */
		sw.src.sa.press = 1;
		sw.src.sa.latch = latch - 10;
	} else {
		sw.src.sa.latch = latch;
		sw.src.sa.press = 0;
	}
	sw.dst.da.bus = (uint8_t)(vals[3] & 0xff);
	sw.dst.da.adr = (uint8_t)(vals[4] & 0xff);
	sw.dst.da.pio = (uint8_t)(vals[5] & 0xff);

	// optional, a timed switch: [secs] [on_press]
	// on_press: 0 a press restarts the timer, 1 it switches off
	// Only taken when it is a number, so other trailing text is still
	// ignored as before.
	int opt[2] = { 0, SW_ON_PRESS_RETRIGGER };
	for (int i = 0; i < 2; ++i) {
		while (ptr < end && std::isspace(*ptr)) ptr++;
		if (ptr == end || !std::isdigit(*ptr))
			break;
		auto [next, ec] = std::from_chars(ptr, end, opt[i]);
		if (ec != std::errc{})
			break;
		ptr = next;
	}
	if (opt[0] > 0xffff || opt[1] > SW_ON_PRESS_OFF) {
		logger.warn (std::format("timed switch out of range {}", buf));
		return false;
	}
	sw.secs = (uint16_t)opt[0];
	sw.on_press = (uint8_t)opt[1];

	return true;
}

SwitchHandler::SwitchHandler()
{
	cur_latch = 0;
	this->ds = nullptr;
	mode = MODE_ALRAM_HANDLING | MODE_ALRAM_POLLING | MODE_AUTO_SWITCH;
}

SwitchHandler::SwitchHandler(OwDevices* devs)  : SwitchHandler()
{
	this->ow = devs;
}

uint8_t SwitchHandler::bitnumber()
{
	int i, res = 1;

	for (i = 0x1; i <= 0x80; i = i << 1) {
		if (cur_latch & i) {
			cur_latch -= i;
			return res;
		}
		res++;
	}
	/* invalid */
	return 0xff;
}

void SwitchHandler::begin(DS2482 *ds)
{
	this->ds = ds;
	logger.info("starting switch handler");
}

/* Convert from alarm location to a lookup table format (16 bit)
 * latch - bit mask to be converted to bit number
 * press - 0: pressing, > 0: press time
 *
 * latch = cur_latch (from data[2])
 * press = data[6]
 */
uint16_t SwitchHandler::srcData(uint8_t busNr, uint8_t adr1)
{
	union s_adr src;
	uint8_t nr;

	src.data = 0;
	src.sa.bus = busNr;
	src.sa.adr =  adr1 & 0x3f;
	// get (the first if multiple) bit which is set
	// TODO returns 0xff if invalid -> check here
	// or return immediately if cur_latch == 0
	nr = bitnumber();
	if (nr == 0xff) {
		logger.warn(std::format("invalid latch data {}.{}", (int)src.sa.bus, (int)src.sa.adr));
		return 0;
	}
	src.sa.latch = nr;
	//v = ow->getVersion(src.sa.bus, src.sa.adr);
	if (data[6] == 0xff)
		src.sa.press = 0;
	else {
		/* first two latches are usually output
		* signaled from auto switch. The corresponding latch
		* is not signaled!
		* */
		if (data[6] == 0)
			src.sa.press = 2;
		else if (data[6] > (350 / 32))
			src.sa.press = 1;
		else
			src.sa.press = 0;
	}
	logger.verbose(std::format("src {}.{}.{}.{}", (int)src.sa.bus, (int)src.sa.adr, (int)src.sa.latch, (int)src.sa.press));
#if 0
	printf("%d.%d.", src.sa.bus, src.sa.adr);
	if (src.sa.press)
		printf("%d ", 10 * src.sa.press + src.sa.latch);
	else
		printf("%d ", src.sa.latch);
	if (data[6] != 0xff) {
		printf(" time=%d", data[6] * 32);
	}
	printf("\n");
#endif

	return src.data;
}

/* Reads out PIO data and checks for dimmer, calls toggle or set
   functions for PIO or dimmer.
 @return true if successfully switched, false in case of an error or
 no action was needed (if switch is already in that state)
*/
bool SwitchHandler::actor_handle(union pio p, enum _pio_mode state)
{
	bool ret = false;

	ds2408* dev = (ds2408*)ow->find(p.da.bus, p.da.adr, 0x29);
	ds->log_event('2',p.da.pio);

	if (dev)
		ret = dev->pin_switch(p.da.pio, state);
	else
		logger.debug("dev not found...");

	return ret;
}

/* latch in cur_latch - bit mask to be converted to bit number
   uses data[1] for actual IO status
   data[6] for press time */
bool SwitchHandler::switchHandle(uint8_t busNr, uint8_t adr1)
{
	union s_adr src;
	size_t i;

	src.data = srcData(busNr, adr1);
#if 0
	for (i = 0; i < MAX_TIMED_SWITCH; i++) {
	}
#endif
	for (i = 0; i < cache.switches.size(); i++) {
		if (src.data == cache.switches[i].src.data) {
			logger.debug(std::format("sw {}.{}.{} -> {}.{}.{}",
				(int)cache.switches[i].src.sa.bus,
				(int)cache.switches[i].src.sa.adr,
				(int)(cache.switches[i].src.sa.latch + cache.switches[i].src.sa.press * 20),
				(int)cache.switches[i].dst.da.bus,
				(int)cache.switches[i].dst.da.adr,
				(int)cache.switches[i].dst.da.pio
			));
			if (cache.switches[i].secs) {
				switch_timed(cache.switches[i]);
			} else {
				/* toggle io or select levels */
				actor_handle(cache.switches[i].dst, TOGGLE);
				// a plain button decides for good, on or off: a timer
				// some timed button started before is done either way
				timer_stop(cache.switches[i].dst);
			}
		}
	}
	return false;
}

bool SwitchHandler::output_on(union pio dst)
{
	ds2408* dev = (ds2408*)ow->find(dst.da.bus, dst.da.adr, 0x29);

	return dev && dev->pin_is_on(dst.da.pio);
}

/* A press of a timed switch. Off: switches the target on and starts
   its timer - only that starts a timer. On: depends on the switch's
   on_press
   - SW_ON_PRESS_OFF: switches it off (and stops its timer, if any),
     also when something else switched it on
   - SW_ON_PRESS_RETRIGGER: restarts its timer; when something else
     switched it on (no timer runs) it is left on, no timer started */
void SwitchHandler::switch_timed(const struct _sw_tbl& sw)
{
	if (!output_on(sw.dst)) {
		// a timer left over from before it was switched off elsewhere
		// must not decide below
		timer_stop(sw.dst);
		actor_handle(sw.dst, ON);
		timer_start(sw.dst, sw.secs);
		return;
	}
	if (sw.on_press == SW_ON_PRESS_OFF) {
		timer_stop(sw.dst);
		actor_handle(sw.dst, OFF);
		return;
	}
	if (!timer_running(sw.dst)) {
		logger.verbose(std::format("{}.{}.{} already on, no timer",
			(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
		return;
	}
	// retrigger: stays on, counts from now again
	timer_start(sw.dst, sw.secs);
}

void SwitchHandler::timer_start(union pio dst, uint16_t secs)
{
	std::lock_guard<std::mutex> lk(timers_mtx);
	timers[dst.data] = HrClock::now() + std::chrono::seconds(secs);
	logger.verbose(std::format("timer {}.{}.{} off in {}s",
		(int)dst.da.bus, (int)dst.da.adr, (int)dst.da.pio, secs));
}

bool SwitchHandler::timer_stop(union pio dst)
{
	std::lock_guard<std::mutex> lk(timers_mtx);
	return timers.erase(dst.data) > 0;
}

bool SwitchHandler::timer_running(union pio dst)
{
	std::lock_guard<std::mutex> lk(timers_mtx);
	return timers.count(dst.data) > 0;
}

int SwitchHandler::timer_remaining(union pio dst) const
{
	std::lock_guard<std::mutex> lk(timers_mtx);
	auto it = timers.find(dst.data);
	if (it == timers.end())
		return -1;
	auto left = std::chrono::duration_cast<std::chrono::seconds>(it->second - HrClock::now()).count();
	return left > 0 ? (int)left : 0;
}

int SwitchHandler::timer_poll(HrClock::time_point now)
{
	std::vector<union pio> expired;

	{
		std::lock_guard<std::mutex> lk(timers_mtx);
		for (auto it = timers.begin(); it != timers.end();) {
			if (now >= it->second) {
				union pio p;
				p.data = it->first;
				expired.push_back(p);
				it = timers.erase(it);
			} else {
				++it;
			}
		}
	}
	// switched outside timers_mtx, see there
	for (const auto& p : expired) {
		logger.verbose(std::format("timer {}.{}.{} expired, off",
			(int)p.da.bus, (int)p.da.adr, (int)p.da.pio));
		actor_handle(p, OFF);
	}
	return (int)expired.size();
}

bool SwitchHandler::dev_alarm(uint8_t bus, uint8_t adr[8])
{
	if (adr[0] == 0x29) {
		uint8_t res, to = 8;
		ds2408* dev = (ds2408*)ow->find(bus, adr[1], 0x29);
		if (!dev)
			return false;
		ds->log_event('1',adr[1]);
		uint8_t stat, press;
		{
			// the read and the copy out as one: data[] is the device's,
			// guarded by its lock. Not held for switching below, which
			// locks the target devices.
			auto lk = dev->lock();
			// also keeps the latches and flags the device; the read
			// resets them on the device, so work on the copy handed
			// back, see alarm_read()
			res = dev->alarm_read(cur_latch);
			stat = dev->data[STAT];
			press = dev->data[PIO_TIME];
		}
		/* fill data for use in switchHandle */
		if (res == 0xaa || res == 0xff) {
			// loop over all set bits
			// cur_latch is reduced by each call to bitnumber
			//logger.verbose(std::format(" -> {} {}", cur_latch, data[5]));
			if (stat & 0x40) {
				/* status in 5 signals a dimming down */
			}
			if (stat & 0x88) {
				logger.warn(std::format("Watchdog on {}!", dev->rom));
				return false;
			}
			if (cur_latch == 0xff) {
				logger.warn(std::format("{}: invalid latch data", dev->rom));
				return false;
			}
			while (cur_latch != 0 && to > 0) {
				data[6] = press;
				switchHandle(bus, adr[1]);
				to--;
			}
		}
		return true;
	}
	auto* dev = ow->find(bus, adr[1], adr[0]);
	if (!dev)
		return false;
	dev->alarm = true;
	dev->poll();

	return true;
}

// FS entries
std::vector<string> SwitchHandler::fs_dir(string& path) const
{
	return fs_table::dir(*this, table, n_table, path);
}

int SwitchHandler::fs_attr(string& path) const
{
	int r = fs_table::attr(*this, table, n_table, path);
	return r == fs_table::NOT_FOUND ? -1 : r;
}

int SwitchHandler::fs_open(string& path) const
{
	int r = fs_table::open(table, n_table, path);
	return r == fs_table::NOT_FOUND ? -ENOENT : r;
}

/* The latch as written in /switches/add: 1..8 short press, 11..18
   long press released, 21..28 long press started (see parse_buf) */
static int latch_code(union s_adr src)
{
	switch (src.sa.press) {
		case 1: return src.sa.latch + 10;
		case 2: return src.sa.latch + 20;
		default: return src.sa.latch;
	}
}

/* "<device name>, <pin name> (latch 3, long)" */
std::string SwitchHandler::src_label(union s_adr src) const
{
	ds2408* dev = (ds2408*)ow->find(src.sa.bus, src.sa.adr, 0x29);
	std::string s = dev ? dev->display_name() + ", " + dev->pin_label(src.sa.latch - 1)
		: std::format("unknown device {}.{}", (int)src.sa.bus, (int)src.sa.adr);

	s += std::format(" (latch {}", (int)src.sa.latch);
	if (src.sa.press == 1)
		s += ", long";
	else if (src.sa.press == 2)
		s += ", long started";
	return s + ")";
}

/* "<device name>, <pin name>" */
std::string SwitchHandler::dst_label(union pio dst) const
{
	ds2408* dev = (ds2408*)ow->find(dst.da.bus, dst.da.adr, 0x29);

	if (!dev)
		return std::format("unknown device {}.{}", (int)dst.da.bus, (int)dst.da.adr);
	return dev->display_name() + ", " + dev->pin_label(dst.da.pio);
}

/* One line per switch: first how it is added (so it can be copied to
   /switches/add or del as is), then the device and pin names and for
   a timed switch its timing and, while it runs, the target's timer:

   1.2.5 -> 2.2.1 30 0: Hallway, Button (latch 5) -> Stairs, Light (PIO.1), timed 30s, press restarts, off in 12s
*/
std::string SwitchHandler::list_text() const
{
	std::string list = std::format("{} switches\n", cache.switches.size());

	for (const auto& sw : cache.switches) {
		list += std::format("{}.{}.{} -> {}.{}.{}",
			(int)sw.src.sa.bus, (int)sw.src.sa.adr, latch_code(sw.src),
			(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio);
		if (sw.secs)
			list += std::format(" {} {}", sw.secs, sw.on_press);
		list += ": " + src_label(sw.src) + " -> " + dst_label(sw.dst);
		if (sw.secs) {
			list += std::format(", timed {}s, press {}", sw.secs,
				sw.on_press == SW_ON_PRESS_OFF ? "off" : "restarts");
			int left = timer_remaining(sw.dst);
			if (left >= 0)
				list += std::format(", off in {}s", left);
		}
		list += "\n";
	}
	return list;
}

int SwitchHandler::list_size(int) const
{
	return (int)list_text().size();
}

int SwitchHandler::r_list(char* buf, size_t size, bool, int)
{
	std::string list = list_text();

	std::snprintf(buf, size, "%s", list.c_str());
	return std::strlen(buf);
}

int SwitchHandler::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	int r = fs_table::read(*this, table, n_table, path, buf, size, uncached);
	// unmatched (e.g. "add"/"del", both write-only) reads as empty,
	// same as this class's own fs_read() always did
	return r == fs_table::NOT_FOUND ? 0 : r;
}

int SwitchHandler::w_add(const char* buf, size_t size, int)
{
	int start = 0;

	for (size_t i = 0; i < size; i++) {
		if (buf[i] == '\n' || buf[i] == '\0') {
			struct _sw_tbl sw;
			if (parse_buf(std::string_view(&buf[start], i - start), sw)) {
				start = i + 1;
				bool exists = false;
				for (auto& sw_i : cache.switches) {
					if (sw.src.data == sw_i.src.data &&
						sw.dst.data == sw_i.dst.data) {
						// adding it again sets its timing anew
						sw_i.secs = sw.secs;
						sw_i.on_press = sw.on_press;
						exists = true;
						break;
					}
				}
				if (exists) {
					logger.info(std::format("switch {}.{}.{} -> {}.{}.{} already exists, timing updated",
						(int)sw.src.sa.bus, (int)sw.src.sa.adr, (int)sw.src.sa.latch,
						(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
					continue;
				}
				cache.switches.push_back(sw);
				logger.verbose(std::format("added switch {}.{}.{} -> {}.{}.{}",
					(int)sw.src.sa.bus, (int)sw.src.sa.adr,
					(int)(sw.src.sa.latch + sw.src.sa.press * 20),
					(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
			}
		}
	}
	return size;
}

int SwitchHandler::w_del(const char* buf, size_t size, int)
{
	int start = 0;
	bool found = false;

	for (size_t i = 0; i < size; i++) {
		if (buf[i] == '\n' || buf[i] == '\0') {
			struct _sw_tbl sw;
			logger.verbose(std::format("adding? {}", std::string(buf)));
			if (parse_buf(std::string_view(&buf[start], i - start), sw)) {
				start = i + 1;
				for (auto it = cache.switches.begin();
					it != cache.switches.end();) {
					const auto& sw_i = *it;
					if (sw.src.data == sw_i.src.data &&
						sw.dst.data == sw_i.dst.data) {
						// remove; erase() invalidates it, so the
						// returned (still valid) iterator must be
						// what we continue from
						it = cache.switches.erase(it);
						logger.verbose(std::format("deleted switch {}.{}.{} -> {}.{}.{}",
							(int)sw.src.sa.bus, (int)sw.src.sa.adr,
							(int)(sw.src.sa.latch + sw.src.sa.press * 20),
							(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
						found = true;
					} else
						++it; // Only increment if we didn't erase
				}
			}
		}
	}
	if (!found) {
		logger.warn("switch entry not found");
		return 0;
	}
	return size;
}

int SwitchHandler::fs_write(string& path, const char* buf, size_t size)
{
	int r = fs_table::write(*this, table, n_table, path, buf, size);
	// unmatched (e.g. "list", read-only) silently accepts the write,
	// same as this class's own fs_write() always did
	return r == fs_table::NOT_FOUND ? (int)size : r;
}
