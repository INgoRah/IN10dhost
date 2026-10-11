#ifndef _DS2408_H
#define _DS2408_H
#ifdef TESTING
#include <gtest/gtest_prod.h>
#endif
#include <array>
#include <string>
#include "ow_dev.h"
#include "interface/ds2408.h"
#include "fs_table.h"
#include "interface/pin_role.h"

#define DS2408_CFG_SIZE 26
/** longest pin name, same as the size its fs entry reports */
#define PIN_NAME_MAX 20

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
		// PIO.* of all pins as a bit mask, bit set = on (see pin_is_on())
		uint8_t pio_mask() const;
		// switches every pin to the state its bit in "on" asks for
		void pio_mask_set(uint8_t on);
		int r_cfg(char* buf, size_t size, bool uncached, int idx);
		int w_cfg(const char* buf, size_t size, int idx);
		int r_pin_name(char* buf, size_t size, bool uncached, int idx);
		int w_pin_name(const char* buf, size_t size, int idx);
		int r_pin_func(char* buf, size_t size, bool uncached, int idx);
		int w_pin_func(const char* buf, size_t size, int idx);
		int r_pin_role(char* buf, size_t size, bool uncached, int idx);
		int w_pin_role(const char* buf, size_t size, int idx);
		int r_pin_room(char* buf, size_t size, bool uncached, int idx);
		int w_pin_room(const char* buf, size_t size, int idx);
		int r_threshold(char* buf, size_t size, bool uncached, int idx);
		int w_threshold(const char* buf, size_t size, int idx);
		int r_brightness(char* buf, size_t size, bool uncached, int idx);
		int w_brightness(const char* buf, size_t size, int idx);
		int r_level(char* buf, size_t size, bool uncached, int idx);
		int w_level(const char* buf, size_t size, int idx);
		// PIO.* only lists the pins currently configured as an output,
		// sensed.* only the ones configured as an input/button
		bool vis_pio(int idx) const;
		bool vis_sensed(int idx) const;
		// level.* only for pins configured as PWM output
		bool vis_level(int idx) const;
		bool vis_latched(int idx) const;
		// pin.0 .. pin.7 are directories purely because two of these
		// rows exist - see the "pin dot star slash ..." comment on
		// FsEntry in fs_table.h. There is no separate "pin.*" row for
		// the directory itself.
		static const FsEntry<ds2408> table[];
		static const size_t n_table;
		int threshold = 0;
		int brightness = 0;
		// user given name per pin, empty means the default "n.<pin>"
		std::array<std::string, 8> pin_name;
		// what is connected to a pin and where, see pin_role.h; only
		// kept here, never written to the device
		std::array<PinRole, 8> pin_role{};
		std::array<std::string, 8> pin_room;
		// activity latches collected by alarm_read() since they were last
		// cleared by writing to a latch.* entry; other register reads
		// (data[PIO_LATCH]) do not change it
		uint8_t latched = 0;
		// activity latches as reg_read() got them, before it reset them
		// on the device (which also clears data[PIO_LATCH])
		uint8_t last_latch = 0;
		// level per pin in percent as last applied by level_set()
		std::array<uint8_t, 8> level_pct{};
		// data[PIO_OUT] holds what the device really drives, not the
		// zeroes (= all on) it starts with
		bool out_known = false;
		// reg_read() saw the watchdog status, the outputs the device
		// had before it, for watchdog_restore()
		bool wdt_pending = false;
		uint8_t wdt_out = 0;
		bool wdt_out_known = false;
		// watchdog resets seen since the start
		unsigned wdt_count = 0;
		int r_watchdog(char* buf, size_t size, bool uncached, int idx);
#ifdef TESTING
		// GTest testing support: the status register the simulated
		// bus reports, e.g. STAT_WATCHDOG
		uint8_t sim_stat = 0;
		// and whether its PIO and level writes fail
		bool sim_fail = false;
		FRIEND_TEST(FsTest, Ds2408Watchdog);
#endif
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
		uint8_t data[10] = {};
		using OwDev::OwDev;
		using OwDev::type;
		void begin(bool soft=false) override;

		ds2408() { type = "ds2408"; };
		ds2408(std::string rom) : OwDev(std::move(rom)) {
			type = "ds2408"; };
		uint8_t cfg[DS2408_CFG_SIZE] = {};

		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size,
			bool uncached) override;
		int fs_write(string& path, const char* buf, size_t size) override;
		json to_json() const override;
		virtual void from_json(const json& j);

		// ds2408 functions
		uint8_t pio_set(uint8_t pio);
		uint8_t reg_read(bool latch_reset);
		uint8_t alarm_read(uint8_t& latch);
		uint8_t latch_reset();
		int cfg_read();
		int cfg_write(const uint8_t* data, int len = DS2408_CFG_SIZE);
		uint8_t level_set(uint8_t pio, uint8_t level,
			uint8_t cmd = TMR_TYPE_ON, uint8_t val = 0);
		uint8_t pin_switch(uint8_t pio, enum _pio_mode state, uint8_t lvl = 0);
		// whether an output is on as far as known here: a PWM pin with a
		// level above 0, otherwise its output latch bit cleared (active
		// low, see pin_switch())
		bool pin_is_on(uint8_t pio) const;
		// the name given to a pin, or "PIO.<n>" while it has none
		std::string pin_label(int pio) const;
		PinRole role(int pio) const;
		// configured as an output (plain or PWM), see vis_pio()
		bool pin_is_output(int pio) const;
		// appends the pins that have a role, all of them for
		// PinRole::none; see IDevices::pins()
		void pins_get(std::vector<PinInfo>& out, PinRole role) const;
		// switches the output pins with that role, see
		// IDevices::role_switch(); returns how many failed
		int role_switch(PinRole role, bool on);
		// the room given to a pin, empty while it has none
		std::string room(int pio) const;
		int brightness_set(uint8_t brightness);
		int threshold_set(uint8_t threshold);
		int get_brightness() const;
		int get_threshold() const;
		// a register read saw the watchdog status, not yet restored
		bool watchdog_pending() const;
		/* After a watchdog reset: writes the cached state back - the
		   outputs as before the reset, the PWM levels, threshold and
		   brightness. Returns 0, or -EAGAIN if a part failed. */
		int watchdog_restore();
};
#endif
