#ifndef _SWITCHHANDLER_H
#define _SWITCHHANDLER_H
#ifdef TESTING
#include <gtest/gtest_prod.h>
#endif
#include "ds2482.h"
#include "ds2408.h"
#include "fs.h"

using std::string;

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

struct _sw_tbl {
	union s_adr src;
	union pio dst;
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
		OwDevices* ow;
		DS2482 *ds;
		uint8_t data[10];
		// current latch data to be hanled
		uint8_t cur_latch;
  		uint16_t srcData(uint8_t busNr, uint8_t adr1);
		uint8_t dataRead(union pio dst, uint8_t adr[8]);
		uint8_t dimStage(uint8_t dim);
		uint8_t getType(union pio dst);
		uint8_t bitnumber();
		bool timerUpdate(union d_adr_8 dst, uint8_t typ);
		uint8_t dimLevel(union pio dst, uint8_t* id);
		uint8_t dimLevel(union d_adr_8 dst, uint8_t* id);
		uint16_t getLen(uint8_t max, uint16_t elSize);
#ifdef TESTING
		// GTest testing support
		FRIEND_TEST(LLSwTest, ll_funcs);
#endif
	public:
		uint8_t mode;
		uint8_t light_thr;
		uint8_t dim_on_lvl;
		uint8_t tbl_vers;
		SwitchHandler();
		SwitchHandler(OwDevices* devs);
		void status();
		bool actor_handle(union pio dst, enum _pio_mode state);
		void begin(DS2482 *ow);
		void loop();
		bool dev_alarm(uint8_t bus, uint8_t adr[8]);
		bool alarmHandler(uint8_t busNr);
		bool switchHandle(uint8_t busNr, uint8_t adr1);
		bool switchHandle(uint8_t busNr, uint8_t adr1, uint8_t latch);
		bool switchLevel(union pio dst, uint8_t level);
		bool initialStates();
		// FS entries
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_open(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;
};
#endif