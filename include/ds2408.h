#ifndef _DS2408_H
#define _DS2408_H
#include "ow_dev.h"
#include "interface/ds2408.h"

#define CFG_SIZE 26

class ds2408 : public OwDev, public IDS2408 {
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
		uint8_t cfg[CFG_SIZE];

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
		int cfg_write(int len = CFG_SIZE);
		uint8_t level_set(uint8_t pio, uint8_t level, uint8_t cmd = TMR_TYPE_ON, uint8_t val = 0);
		uint8_t pin_switch(uint8_t pio, enum _pio_mode state, uint8_t lvl = 0);
		uint8_t ard_set(uint8_t pio, uint8_t val);
};
#endif
