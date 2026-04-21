#pragma once

#include <string>
#include <stdint.h>
#include <mutex>

#define stOk 0
#define stTimeout 1
#define stNoDevs 2
#define stBusy 3

#define OW_MATCH_ROM    0x55
#define OW_SKIP_ROM     0xCC
#define OW_SEARCH_ROM   0xF0
#define OW_COND_SEARC_ROM  0xEC
/* start new search */
#define OW_SEARCH_FIRST 0xFF

#define DS2482_CONFIG_APU (1<<0)
#define DS2482_CONFIG_PPM (1<<1)
#define DS2482_CONFIG_SPU (1<<2)
#define DS2484_CONFIG_WS  (1<<3)

#define DS2482_STATUS_BUSY 	0x01
/* presence bit */
#define DS2482_STATUS_PPD 	0x02
#define DS2482_STATUS_SD	0x04
#define DS2482_STATUS_LL	0x08
#define DS2482_STATUS_RST	0x10
#define DS2482_STATUS_SBR	0x20
#define DS2482_STATUS_TSB	0x40
#define DS2482_STATUS_DIR	0x80
#define DS2482_STATUS_INVAL	0xfd

enum DS2482_ERR {
  ERR_NONE = 0,
  /* length to long for buffer, should never occur */
  ERR_WIRE_LEN = 1,
  /* address send, NACK received */
  ERR_WIRE_ADRNACK = 2,
  /* data send, NACK received */
  ERR_WIRE_DATANACK = 3,
  /* other twi error (lost bus arbitration, bus error, ..) */
  ERR_WIRE_GEN = 4,
  /* Timeout */
  ERR_WIRE_TO = 5,
  /* requestFrom return 0: no data cause of timeout */
  ERR_READ = 6,
  ERR_WRITE = 7,

  ERR_BUSYWAIT = 10,
  /* 12, 13, 14, 15 */
  ERR_BUSYWAIT_RD = 17,
  ERR_BUSYWAIT_TO = 18,

  ERR_CHSEL1 = 20,
  /* ..1, ..2, ..3, ..4, ..5 */
  ERR_CHSEL2 = 40,
  /* ..1, ..2, ..3, ..4, ..5 */
  ERR_CHCHK = 47,

  ERR_RESET1 = 40,
  ERR_RESET2 = 60,
  ERR_RESET3 = 55,

  ERR_WRITE1 = 64,
  ERR_WRITE2 = 82,
  ERR_READ1 = 80,
  ERR_READ2 = 85,
  ERR_READ3 = 90,
  ERR_SELECT1 = 30, /* = 100 - 110 (30 + write 1,2) */
  ERR_SELECT2 = 40, /* = 110 - 120 (40 + write 1,2) */
  ERR_SRCH1 = 100, /* (reset + 100) */
  ERR_SRCH2 = 140,  /* write + 150 */
  ERR_NEXT
};

class DS2482
{
public:
	DS2482() { fd = -1; };
	DS2482(const std::string& i2c_dev, int address);
	~DS2482();

	std::mutex mtx;
	uint8_t last_err;
	bool init();
	bool configureDev(uint8_t config);
	void resetDev();

	bool reset(); // return true if presence pulse is detected
	bool selectChannel(uint8_t channel);

	uint8_t write(uint8_t b, uint8_t power = 0 );
	uint8_t read();
	void read(uint8_t *buf, uint16_t count);
	void write(const uint8_t *buf, uint16_t count, uint8_t power/* = 0 */);
	// Issue a 1-Wire rom select command, you do the reset first.
	void select(const  uint8_t rom[8]);
	// Issue skip rom
	void skip();

	// Clear the search state so that if will start from the beginning again.
	void reset_search();
	void target_search(uint8_t family_code);

	// Look for the next device. Returns 1 if a new address has been
	// returned. A zero might mean that the bus is shorted, there are
	// no devices, or you have already retrieved all of them.  It
	// might be a good idea to check the CRC to make sure you didn't
	// get garbage.  The order is deterministic. You will always get
	// the same devices in the same order.
	bool search(uint8_t *newAddr, bool search_mode = true);
	uint16_t crc16(const uint8_t* input, uint16_t len, uint16_t crc);
	bool check_crc16(const uint8_t* input, uint16_t len, const uint8_t* inverted_crc, uint16_t crc);

private:
	int fd;
	uint8_t addr;
	uint8_t ch;
	uint8_t _read_ptr;
	uint8_t searchLastDisrepancy;
	uint8_t searchLastFamilyDiscrepancy;
	uint8_t searchAddress[8];
	uint8_t searchExhausted;

	void set_error(int err_code, int def);
	void _write(uint8_t b);
	void _write_cmd(uint8_t cmd, uint8_t data);
	int read_status(uint8_t *status);
	uint8_t _read();
	void setReadPtr(uint8_t readPtr);

	uint8_t busyWait(); //blocks until
};
