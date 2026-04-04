#include "ow_devices.h"

#define CFG_SIZE 26

/* Commands available in pin timer command */
/** Configure brigthness if no sensor available */
#define TMR_TYPE_BRIGHTNESS 0xE3
/** Configure brigthness threshold when to switch */
#define TMR_TYPE_THRESHOLD 0xE5

/** Switch on without timer (force on) */
#define TMR_TYPE_ON 0xDD
/** Switch on without timer (force on) with dim up */
#define TMR_TYPE_ON_DIM 0xDB
/** Switch off and stop timer if running (force off) */
#define TMR_TYPE_STOP 0xEE
/** Switch off with dim down and stop timer if running (force off) */
#define TMR_TYPE_STOP_DIM 0xEB

/* register 0: PIO Logic State  = sensed
   real logic level no matter of input or output */
#define PIO_LS 0
/* register 1: Output latch = PIO
   Actively driving the PIO to 0 (or not=input) if output */
#define PIO_OUT 1
/* register 2: Active alarm latch state = latch
   Signaling a change */
#define PIO_LATCH 2
/* register 2: Active alarm latch state = latch
   Signaling a change */
#define STAT 5

class ds2408 : public OwDev {
	private:
	public:
		/* [0] PIO Logic State  = sensed
		 * [1] Output latch = PIO
		 * [2] Activity latch state = latch
		 * [3] Conditional search channel selection (unused)
		 * [4] Conditional search polarity selection (unused)
		 * [5] Status
		 * [6] Status ext 1, used for press time detection
		 * [7] Status ext 2
		 * [8] CRC16 LSB
		 * [9] CRC16 MSB
		 **/
		uint8_t data[10];
		using OwDev::OwDev;
		using OwDev::type;
		ds2408() { this->val = 0x55; this->mode = 0; };
		ds2408(std::string rom) : OwDev(rom) { type = "ds2408"; };
		int val;
		uint8_t cfg[24];

		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached) override;
		int fs_write(string& path, const char* buf, size_t size) override;
		json to_json() const override;
		virtual void from_json(const json& j);

		// ds2408 functions
		void update(uint8_t pio, uint8_t ff1);
		uint8_t pio_set(uint8_t pio);
		uint8_t reg_read(bool latch_reset);
		uint8_t latch_reset();
		int cfg_read();
		int cfg_write();
		uint8_t xpin_set(uint8_t pio, uint8_t level, uint8_t cmd, uint8_t val);
		uint8_t ard_set(uint8_t pio, uint8_t val);
};
