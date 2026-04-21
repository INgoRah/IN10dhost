#ifndef _DS2408_H
#define _DS2408_H
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
/* register 5: Status */
#define STAT 5
/* register 6: reserved, but used for press time */
#define PIO_TIME 6

/*
 * PIO configuration
 */
#define CFG_PIN_ID 11 /* .. 18 */
#define CFG_DEFAULT 0xff /* =0x10 / press button */
#define CFG_UNUSED 0
/* input */

/** Signal is active high, no pull up. Touch input with short high output.
 * Reacts only on rising edge and thus does not support auto switch */
#define CFG_ACT_HIGH 2
/** Signal is active low, no pull up. Input with short low output.
 * Reacts only on falling edge and thus does not support auto switch */
#define CFG_ACT_LOW 3
/** Active low with pull up */
#define CFG_ACT_LOW_PU 4
/** Level is passed as is, alarm on change */
#define CFG_PASS 5
/** Level is passed with inverted logic, alarm on change.
 * Level 0 activates the output, level 1 deactivates it
 */
#define CFG_PASS_INV 6
/** Same as CFG_PASS_INV but enables a pull up */
#define CFG_PASS_INV_PU 7
/** Same as CFG_PASS but enables a pull up */
#define CFG_PASS_PU 8
#define CFG_BTN_MASK 0x10 /** button mask */
/** Press button with validation and long press detection.
 * A 1 represents normal polarity (push button to ground) with
 * pull-up resistor. High is inactive, low pushed */
#define CFG_BTN 0x10

#define CFG_SW 0x11 /** switch button */

/* output */
#define CFG_OUT_MASK 0x20

/** Active low output (default DS2408 behaviour).
 * Represents normal polarity (open collector)
 * Set PIO to 0 = conducting, on, pin active low
 * set 1 / non-conducting (off), input
*/
#define CFG_OUT_LOW 0x21

/** Active high output. Used for output via transistor.
 * Set 0 = on, pin is active high (no input)
 * Set 1 = off, inactive, pin is in high resistive not
 *         driving high, no pullup
 * Logic state represents the transistor output, not pin output
 */
#define CFG_OUT_HIGH 0x22
#define CFG_OUT_PWM 0x23

enum _pio_mode {
	OFF,
	LEVEL,
	ON,
	TOGGLE
};


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
		ds2408() { this->level = 0; this->mode = 0; };
		ds2408(std::string rom) : OwDev(rom) { type = "ds2408"; };
		// each custom device may have one PWM output enabled and
		// can set one dedicated pin to any level
		// the level is set with a specail custom command
		int level;
		uint8_t cfg[24];

		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached) override;
		int fs_write(string& path, const char* buf, size_t size) override;
		json to_json() const override;
		virtual void from_json(const json& j);

		// ds2408 functions
		uint8_t pio_set(uint8_t pio);
		uint8_t reg_read(bool latch_reset);
		uint8_t latch_reset();
		int cfg_read();
		int cfg_write();
		uint8_t level_set(uint8_t pio, uint8_t level, uint8_t cmd = TMR_TYPE_ON, uint8_t val = 0);
		uint8_t pin_switch(uint8_t pio, enum _pio_mode state, uint8_t lvl = 0);
		uint8_t ard_set(uint8_t pio, uint8_t val);
};
#endif
