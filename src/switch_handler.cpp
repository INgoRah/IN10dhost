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

struct _sw_tbl sw_tbl[MAX_SWITCHES];

static struct filetype Switches[] = {
	{ "add", 32 },
	{ "del", 32 },
	{ "list", 1024 },
};

void to_json(json& j, const _sw_tbl& b) {
	j = json{
		{"src_bus", b.src.sa.bus },
		{"src_adr", b.src.sa.adr },
		{"src_latch", b.src.sa.latch },
		{"src_press", b.src.sa.press },
		{"dst_bus", b.dst.da.bus},
		{"dst_adr", b.dst.da.adr},
		{"dst_pio", b.dst.da.pio},
		{"dst_type", b.dst.da.type}
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
}

bool parse_buf(std::string_view buf, struct _sw_tbl& sw)
{
	int vals[6];
	const char* ptr = buf.data();
	const char* end = buf.data() + buf.size();

	if (buf.size() < (5 + 5)) {
		logger.warn (std::format("size mismatch {}", buf));
		return false;
	}
	for (int i = 0; i < 6; ++i) {
		// Skip leading whitespace manually if needed
		while (ptr < end && std::isspace(*ptr)) ptr++;

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
	sw.src.sa.latch = (uint8_t)(vals[2] & 0xff);
	sw.src.sa.press = 0;
	sw.dst.da.bus = (uint8_t)(vals[3] & 0xff);
	sw.dst.da.adr = (uint8_t)(vals[4] & 0xff);
	sw.dst.da.pio = (uint8_t)(vals[5] & 0xff);

	return true;
}

SwitchHandler::SwitchHandler()
{
	cur_latch = 0;
	this->ds = nullptr;
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

void SwitchHandler::begin(DS2482 *ow)
{
	this->ds = ow;
	mode = MODE_ALRAM_HANDLING | MODE_ALRAM_POLLING | MODE_AUTO_SWITCH;
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
	//uint8_t v;

	src.data = 0;
	src.sa.bus = busNr;
	src.sa.adr =  adr1 & 0x3f;
	// get (the first if multiple) bit which is set
	// TODO returns 0xff if invalid -> check here
	// or return immediately if cur_latch == 0
	src.sa.latch = bitnumber();
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
	logger.debug(std::format("switch handling {}.{}", (int)src.sa.bus, (int)src.sa.adr));
#if 0
	for (i = 0; i < MAX_TIMED_SWITCH; i++) {
	}
#endif
	for (i = 0; i < cache.switches.size(); i++) {
		if (src.data == cache.switches[i].src.data) {
			logger.debug(std::format("sw {}.{}.{} -> {}.{}.{}",
				(int)cache.switches[i].src.sa.bus,
				(int)cache.switches[i].src.sa.adr,
				(int)cache.switches[i].src.sa.latch,
				(int)cache.switches[i].dst.da.bus,
				(int)cache.switches[i].dst.da.adr,
				(int)cache.switches[i].dst.da.pio
			));
			/* toggle io or select levels */
			actor_handle(cache.switches[i].dst, TOGGLE);
		}
	}
	return false;
}

bool SwitchHandler::dev_alarm(uint8_t bus, uint8_t adr[8])
{
	if (adr[0] == 0x29) {
		uint8_t res, to = 8;
		ds2408* dev = (ds2408*)ow->find(bus, adr[1], 0x29);
		if (!dev)
			return false;
		//dev->set_alarm(true);
		ds->log_event('1',adr[1]);
		res = dev->reg_read(true);
		/* fill data for use in switchHandle */
		if (res == 0xaa || res == 0xff) {
			// loop over all set bits
			// cur_latch is reduced by each call to bitnumber
			cur_latch = dev->data[PIO_LATCH];
			//logger.verbose(std::format(" -> {} {}", cur_latch, data[5]));
			if (dev->data[STAT] & 0x40) {
				/* status in 5 signals a dimming down */
			}
			if (dev->data[STAT] & 0x88) {
				logger.warn(std::format("Watchdog on {}!", dev->rom));
				return false;
			}
			if (cur_latch == 0xff) {
				logger.warn(std::format("{}: invalid latch data", dev->rom));
				return false;
			}
			while (cur_latch != 0 && to > 0) {
				data[6] = dev->data[PIO_TIME];
				switchHandle(bus, adr[1]);
				to--;
			}
		}
	}
	return true;
}

bool SwitchHandler::alarmHandler(uint8_t busNr)
{
#ifdef USE_I2C
	uint8_t adr[8];
	uint8_t j = 0;
	uint8_t cnt = 10;
	bool ret, srch;

	if (ds == nullptr)
		return false;
	{
		std::lock_guard<std::mutex> lock(ds->mtx);

		ret = ds->selectChannel(busNr);
		if (!ret)
			// this could be a timeout or other issue
			// must be repeated
			return false;
		ds->target_search(0x29);
		// improve time by 1 ms with a familiy search for 0x29 only
		// with custom addresses using one byte ID only
		// at the second byte and the remaining according a
		// defined scheme, we could stop even after one byte search
		srch = ds->search(adr, false);
	}
	while (srch && cnt > 0) {
		j++;
		logger.debug(std::format("Alarm {}.{} {}", busNr, adr[1], adr[2]));
		try {
			dev_alarm(busNr, adr);
		}
		catch (const std::system_error& e) {
			std::cerr << "Caught system error: " << e.what() << '\n';
			std::cerr << "Error code: " << e.code() << '\n';
		}
		cnt--;
#ifdef USE_DEBUG
		if (ds->last_err || cnt == 0)
			printf("Error searching = %d\n", ds->last_err);
#endif
		{
			std::lock_guard<std::mutex> lock(ds->mtx);
			srch = ds->search(adr, false);
		}
	}

	return j > 0 ? true : false;
#else
	(void)busNr;
	return false;
#endif
}

// FS entries
std::vector<string> SwitchHandler::fs_dir(string& path) const
{
	(void)path;
	std::vector<std::string> dir;
	for (const auto& s : Switches)
		dir.push_back(s.name);

	return dir;
}

int SwitchHandler::fs_attr(string& path) const
{
	(void)path;
	for (const auto& s : Switches)
		if (path.find(s.name) != std::string::npos)
			return s.suglen;

	return -1;
}

int SwitchHandler::fs_open(string& path) const
{
	(void)path;
	return 0;
}

int SwitchHandler::fs_read(string& path, char* buf, size_t size, bool uncached)
{
	(void)uncached;
	if (path.find("list") != string::npos) {
		std::string list;
		list += std::format("{} switches\n", cache.switches.size());
		for (const auto& sw : cache.switches) {
			list += std::format("{}.{}.{} {}.{}.{} ({} {})\n",
				sw.src.sa.bus, sw.src.sa.adr, sw.src.sa.latch,
				sw.dst.da.bus, sw.dst.da.adr, sw.dst.da.pio,
				sw.src.data, sw.dst.data);
		}
		std::strncpy(buf, list.c_str(), size);
		return std::strlen(buf);
	}
	return 0;
}

int SwitchHandler::fs_write(string& path, const char* buf, size_t size)
{
	int start = 0;
	bool found = false;

	for (size_t i = 0; i < size; i++) {
		if (buf[i] == '\n' || buf[i] == '\0') {
			struct _sw_tbl sw;
			logger.verbose(std::format("adding? {}", std::string(buf)));
			if (parse_buf(std::string_view(&buf[start], i - start), sw)) {
				start = i + 1;
				if (path.find("add") != string::npos) {
					cache.switches.push_back(sw);
					logger.verbose(std::format("added switch {}.{}.{} -> {}.{}.{}",
						(int)sw.src.sa.bus, (int)sw.src.sa.adr, (int)sw.src.sa.latch,
						(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
					found = true;
				}
				if (path.find("del") != string::npos) {
					for (auto it = cache.switches.begin();
						it != cache.switches.end();) {
						const auto& sw_i = *it;
						if (sw.src.data == sw_i.src.data &&
							sw.dst.data == sw_i.dst.data) {
							// remove
							cache.switches.erase(it);
							logger.verbose(std::format("deleted switch {}.{}.{} -> {}.{}.{}",
								(int)sw.src.sa.bus, (int)sw.src.sa.adr, (int)sw.src.sa.latch,
								(int)sw.dst.da.bus, (int)sw.dst.da.adr, (int)sw.dst.da.pio));
							found = true;
						} else
							++it; // Only increment if we didn't erase
					}
				}
			}
		}
	}
	if (path.find("del") != string::npos && !found) {
		logger.warn("switch entry not found");
		return 0;
	}
	return size;
}
