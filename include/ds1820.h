#include <optional>
#include "ow_devices.h"
#include "interface/ds1820.h"
#include "fs_table.h"
#include <gtest/gtest_prod.h>

#define DS1820_CFG_SIZE 8

/* Faster polling while the temperature changes (e.g. an open window):
   a reading more than fast_delta K off the one before switches to
   polling every poll / fast_factor seconds, until DS1820_FAST_SETTLED
   readings in a row stayed within fast_delta K. Defaults of the json
   config's "fast_delta" and "fast_factor"; a factor of 1 or a delta of
   0 keeps the poll interval as is. */
#define DS1820_FAST_DELTA 0.2f
#define DS1820_FAST_FACTOR 5
#define DS1820_FAST_SETTLED 3
/* A poll starts the conversion and reads it this much later (a DS18B20
   needs up to 750 ms at 12 bit, the owSlave measures once the bus is
   idle for ~100 ms); intervals below twice that convert and read at
   once */
#define DS1820_CONV_MS 1000L
/* A reading that failed (scratchpad CRC wrong, nothing on the bus):
   a new cycle (convert, read) DS1820_RETRY_MS after it, at most
   DS1820_RETRIES times, then the interval again */
#define DS1820_RETRY_MS 1000L
#define DS1820_RETRIES 2

class ds1820 : public OwDev, public IDS1820 {
	private:
		uint8_t scratchPad[9] = {};
		uint8_t cfg[DS1820_CFG_SIZE] = {};
		float temp = 21.5f;
		uint8_t hum = 50;
		// the last temp_read(1) got the scratchpad
		bool temp_ok = false;
		// poll() started a conversion, the next poll reads it
		bool converting = false;
		// the reading of the last cycle failed, retries since
		int retries = 0;
		// adaptive polling, see DS1820_FAST_DELTA
		float fast_delta = DS1820_FAST_DELTA;
		int fast_factor = DS1820_FAST_FACTOR;
		bool fast = false;
		// the reading before, while fast the one the settling counts from
		std::optional<float> last_temp;
		float settle_ref = 0;
		int settled = 0;
		void poll_adapt(float t);
		// reads the last conversion: adapts the polling, logs a change
		void reading();
		// poll_interval, while fast divided by fast_factor
		long interval_ms() const;
		long poll_ms() const override;
		// fs_table.h handlers
		int r_temperature(char* buf, size_t size, bool uncached, int idx);
		int r_humidity(char* buf, size_t size, bool uncached, int idx);
		int r_cfg(char* buf, size_t size, bool uncached, int idx);
		int w_cfg(const char* buf, size_t size, int idx);
		static const FsEntry<ds1820> table[];
		static const size_t n_table;
		FRIEND_TEST(DevTest, Ds1820FastPolling);
		FRIEND_TEST(DevTest, Ds1820PollConvertThenRead);
		FRIEND_TEST(DevTest, Ds1820AlarmReadsOnly);
		FRIEND_TEST(DevTest, Ds1820CrcRetry);
#ifdef TESTING
		// GTest: the next that many scratchpad reads get a wrong CRC
		int sim_crc_fail = 0;
#endif

	public:
		using OwDev::OwDev;
		using OwDev::type;
		ds1820() { temp = 21.5f; hum = 50; type = "ds1820"; };
		ds1820(std::string rom) : OwDev(std::move(rom)) { temp = 21.5f; hum = 50;type = "ds1820"; };
		int poll();
		int alarm_poll() override;

		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;
		void from_json(const json& j) override;
		json to_json() const override;

		// IDS1820
		float temp_read(const uint8_t flag);
		int cfg_read();
		int cfg_write(const uint8_t* data, int len = DS1820_CFG_SIZE);
};
