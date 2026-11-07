#ifndef _SWITCHHANDLER_H
#define _SWITCHHANDLER_H
#ifdef TESTING
#include <gtest/gtest_prod.h>
#endif
#include <atomic>
#include <map>
#include <mutex>
#include "ds2482.h"
#include "ds2408.h"
#include "fs.h"
#include "fs_table.h"

using std::string;

// MAX_SWITCHES below needs MAX_BUS; don't rely on whoever includes
// this header to have already defined it (switch_handler.cpp includes
// this directly, without going through ow_devices.h first).
#ifndef MAX_BUS
#define MAX_BUS 4
#endif

/* config */
#ifdef AVRSIM
#define MAX_TIMER 2
#define MAX_SWITCHES 4
#define MAX_TIMED_SWITCH 3
#else
#define MAX_TIMER 5
/* per bus 8 addresses and each 5 latches, sometimes long presses additionally */
#define MAX_SWITCHES MAX_BUS * 6 * 5
#define MAX_TIMED_SWITCH 6
#endif
#define MAX_DIMMER 4
#define DEF_SECS 30

/* modes */
#define MODE_ALRAM_POLLING 0x2
#define MODE_ALRAM_HANDLING 0x4
/* the switch table is applied; cleared by writing "all" to
   /switches/disable, all presses are ignored then. Saved with the
   config, see SwitchHandler::all_disabled() */
#define MODE_AUTO_SWITCH 0x8

union s_adr {
	uint16_t data;
	struct {
		unsigned int res : 1;
		// pio/latch number 0..7
		unsigned int bus : 3;  /* 1..7 */
		/** Press time
		 * 0: short press
		 * 2: button pushed (for long press detection)
		 * 1: long press released */
		unsigned int press : 2;
		/** Latch number (only one)
		 * 0: invalid
		 * 1..7: valid
		 * */
		unsigned int latch : 4; /* 1..15 */
		unsigned int adr : 6; /* 1..3F (65) */
	} sa;
};

/* custom DS2408 FW with defined address 29 <bus> <adr> ... */
#define TYPE_DS29X 0
/* not used, reserved for standard slaves with ROM address */
#define TYPE_STD1W 1
/* not 1W connected PIOs of own controller */
#define TYPE_INTERN 2

/** target PIO with type and address using the full
 * possible range of PIOs and address. 16 bit */
union pio {
	uint16_t data;
	struct {
		/* PIO0 = 0, PIO1 = 1...PI07 = 7 */
		unsigned int pio : 3;
		unsigned int adr : 8;
		unsigned int bus : 3;
		unsigned int type : 2;
	} da;
};

/* What a press of a timed switch does while the target's timer runs */
/** Restart the timer, the target stays on */
#define SW_ON_PRESS_RETRIGGER 0
/** Switch the target off and stop the timer */
#define SW_ON_PRESS_OFF 1

/* When a switch may switch its target on (_sw_tbl::cond); switching it
 * off is always done */
/** Always */
#define SW_COND_NONE 0
/** Only while it is dark, by the astro flag (/switches/dark), which an
 * external script sets at sunset and sunrise */
#define SW_COND_DARK 1

/* Target 0.0.0 of a switch: every light off (pins with role "light",
 * see /role), instead of one pin */
inline bool is_all_off(union pio dst)
{
	return dst.da.type == TYPE_DS29X && dst.da.bus == 0 && dst.da.adr == 0 && dst.da.pio == 0;
}

struct _sw_tbl {
	union s_adr src;
	union pio dst;
	/** Timed switch: seconds the target stays on after a press, 0 is
	 * a plain switch that toggles the target */
	uint16_t secs = 0;
	/** Timed switch: SW_ON_PRESS_* */
	uint8_t on_press = SW_ON_PRESS_RETRIGGER;
	/** SW_COND_*: when a press may switch the target on */
	uint8_t cond = SW_COND_NONE;
	/** Masked by /switches/disable: its presses are ignored until it
	 * is enabled again. Saved with the switch. */
	bool disabled = false;
};

enum tim_type {
	/** Timer always, hard off per time */
	TYPE_DEF,
	TYPE_20S		/* 1 */,
	TYPE_30S		/* 2 */,
	TYPE_1MIN		/* 3 */,
	TYPE_2MIN		/* 4 */,
	TYPE_5MIN		/* 5 */,
	TYPE_10MIN		/* 6 */,
	TYPE_15MIN		/* 7 */,
	TYPE_30MIN		/* 8 */,
	TYPE_1H 		/* 9 */,
	/** Timer on darkness with hard off per time */
	TYPE_DARK = 10,
	TYPE_DARK_20S	/* 11 */,
	TYPE_DARK_30S	/* 11 */,
	TYPE_DARK_1MIN	/* 12 */,
	TYPE_DARK_2MIN	/* 13 */,
	TYPE_DARK_5MIN	/* 14 */,
	TYPE_DARK_10MIN	/* 15 */,
	TYPE_DARK_15MIN	/* 16 */,
	TYPE_DARK_30MIN	/* 17 */,
	TYPE_DARK_1H 	/* 18 */,
	/** Timer on darkness with blinking off per time */
	TYPE_DARK_BLINK = 30,
	TYPE_DARK_BLINK_30S	/* 31 */,
	TYPE_DARK_BLINK_1MIN	/* 32 */,
	TYPE_DARK_BLINK_2MIN	/* 33 */,
	TYPE_DARK_BLINK_5MIN	/* 34 */,
	TYPE_DARK_BLINK_10MIN	/* 35 */,
	TYPE_DARK_BLINK_15MIN	/* 36 */,
	TYPE_DARK_BLINK_30MIN	/* 37 */,
	TYPE_DARK_BLINK_1H 		/* 38 */,
};

struct _sw_tim_tbl {
	uint8_t type;
	union s_adr src;
	union pio dst;
};

extern struct _sw_tbl sw_tbl[MAX_SWITCHES];
extern struct _sw_tim_tbl timed_tbl[MAX_TIMED_SWITCH];

class OwDevices;
class SwitchHandler : IFs {
	private:
		OwDevices* ow = nullptr;
		DS2482 *ds = nullptr;
		uint8_t data[10] = {};
		// current latch data to be hanled
		uint8_t cur_latch = 0;
  		uint16_t srcData(uint8_t busNr, uint8_t adr1);
		uint8_t dataRead(union pio dst, uint8_t adr[8]);
		uint8_t dimStage(uint8_t dim);
		uint8_t getType(union pio dst);
		uint8_t bitnumber();
		bool timerUpdate(union d_adr_8 dst, uint8_t typ);
		uint8_t dimLevel(union pio dst, uint8_t* id);
		uint8_t dimLevel(union d_adr_8 dst, uint8_t* id);
		uint16_t getLen(uint8_t max, uint16_t elSize);
		// running off timers per target IO (union pio data -> when it is
		// switched off). Per target, not per switch: all switches of one
		// light share its timer. Guarded by timers_mtx, which is never
		// held while switching (that takes the target device's lock).
		std::map<uint16_t, HrClock::time_point> timers;
		mutable std::mutex timers_mtx;
		void switch_timed(const struct _sw_tbl& sw);
		// target 0.0.0, see is_all_off()
		void all_off();
		bool output_on(union pio dst);
		// fs_table.h handlers
		int r_list(char* buf, size_t size, bool uncached, int idx);
		int list_size(int idx) const;
		// what /switches/list shows
		std::string list_text() const;
		std::string src_label(union s_adr src) const;
		std::string dst_label(union pio dst) const;
		int w_add(const char* buf, size_t size, int idx);
		int w_del(const char* buf, size_t size, int idx);
		int r_dark(char* buf, size_t size, bool uncached, int idx);
		int w_dark(const char* buf, size_t size, int idx);
		// the astro flag, see SW_COND_DARK; dark until a script says
		// otherwise, so without one the switches work as before
		std::atomic<bool> dark{true};
		// rooms with a dark flag of their own (/rooms/<room>/dark), by
		// room name (see room_parse()); the others follow the astro flag
		std::map<std::string, bool> room_dark;
		mutable std::mutex dark_mtx;
		// the room of a switch's target pin, empty while it has none
		std::string dst_room(union pio dst) const;
		// whether sw may switch its target on now
		bool cond_met(const struct _sw_tbl& sw) const;
		int w_disable(const char* buf, size_t size, int idx);
		int w_enable(const char* buf, size_t size, int idx);
		// "all" or switch lines as for add/del
		int mask(const char* buf, size_t size, bool disable);
		static const FsEntry<SwitchHandler> table[];
		static const size_t n_table;
#ifdef TESTING
		// GTest testing support
		FRIEND_TEST(LLSwTest, ll_funcs);
#endif
	public:
		uint8_t mode = MODE_ALRAM_HANDLING | MODE_ALRAM_POLLING | MODE_AUTO_SWITCH;
		uint8_t light_thr = 0;
		uint8_t dim_on_lvl = 0;
		uint8_t tbl_vers = 0;
		SwitchHandler();
		SwitchHandler(OwDevices* devs);
		void status();
		bool actor_handle(union pio dst, enum _pio_mode state);
		void begin(DS2482 *ow);
		void loop();
		bool dev_alarm(uint8_t bus, uint8_t adr[8]);
		// a ds2408 reported a watchdog reset, see dev_alarm()
		void dev_watchdog(ds2408* dev);
		bool switchHandle(uint8_t busNr, uint8_t adr1);
		bool switchHandle(uint8_t busNr, uint8_t adr1, uint8_t latch);
		bool switchLevel(union pio dst, uint8_t level);
		// Off timers of target IOs, used by timed switches (and meant for
		// automatic IO timeouts as well): start (or restart) switches dst
		// off after secs, stop cancels that.
		void timer_start(union pio dst, uint16_t secs);
		bool timer_stop(union pio dst);
		bool timer_running(union pio dst);
		// seconds until dst is switched off, -1 while no timer runs
		int timer_remaining(union pio dst) const;
		// Switches off every target whose timer expired by now; called
		// from the poll loop. Returns how many it switched off.
		int timer_poll(HrClock::time_point now = HrClock::now());
		bool initialStates();
		// every switch masked ("all" to /switches/disable), saved with
		// the config as "switches_disabled"
		bool all_disabled() const;
		void all_disabled_set(bool disabled);
		// the astro flag: set by a script at sunset (true) and sunrise.
		// With a room, that room's own flag instead, see room_dark.
		void dark_set(bool dark, const std::string& room = "");
		// the room follows the astro flag again
		void dark_clear(const std::string& room);
		// the flag in effect: the room's own, else the astro flag
		bool is_dark(const std::string& room = "") const;
		// whether the room has a flag of its own
		bool has_dark(const std::string& room) const;
		// the rooms with a flag of their own
		std::vector<std::string> dark_rooms() const;
		// FS entries
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_open(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;
};
#endif