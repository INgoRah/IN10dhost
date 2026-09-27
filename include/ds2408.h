#ifndef _DS2408_H
#define _DS2408_H
#include "ow_dev.h"
#include "interface/ds2408.h"
#include "fs_table.h"

#define CFG_SIZE 26

class ds2408 : public OwDev, public IDS2408 {
	private:
		// fs_table.h handlers, bound to the entries of the private
		// `table` member below (defined out-of-line in ds2408.cpp).
		// Not meant to be called directly.
		int r_byte(char* buf, size_t size, bool uncached, int idx);
		int w_byte(const char* buf, size_t size, int idx);
		int r_pio(char* buf, size_t size, bool uncached, int idx);
		int w_pio(const char* buf, size_t size, int idx);
		int r_sensed(char* buf, size_t size, bool uncached, int idx);
		int r_latched(char* buf, size_t size, bool uncached, int idx);
		int w_latched(const char* buf, size_t size, int idx);
		int r_cfg(char* buf, size_t size, bool uncached, int idx);
		int r_pin_name(char* buf, size_t size, bool uncached, int idx);
		int w_pin_name(const char* buf, size_t size, int idx);
		int r_pin_func(char* buf, size_t size, bool uncached, int idx);
		int w_pin_func(const char* buf, size_t size, int idx);
		// PIO.* only lists the pins currently configured as an output,
		// sensed.* only the ones configured as an input/button
		bool vis_pio(int idx) const;
		bool vis_sensed(int idx) const;
		// pin.0 .. pin.7 are directories purely because two of these
		// rows exist - see the "pin dot star slash ..." comment on
		// FsEntry in fs_table.h. There is no separate "pin.*" row for
		// the directory itself.
		static const FsEntry<ds2408> table[];
		static const size_t n_table;
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
		void begin(bool soft=false) override;

		ds2408() { this->level = 0; type = "ds2408"; };
		ds2408(std::string rom) : OwDev(rom) {
			this->level = 0;
			type = "ds2408"; };
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
};
#endif
