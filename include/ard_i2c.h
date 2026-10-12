#include <stdint.h>
#ifdef TESTING
#include <gtest/gtest_prod.h>
#endif
#include "ow_devices.h"
#include "fs_table.h"

int i2c_write(int fd, uint8_t cmd);
int i2c_write_data(int fd, uint8_t* buf, uint16_t size);
uint8_t i2c_read(int fd);
int i2c_read_data(int fd, uint8_t* buf, uint16_t size);

class Ard_i2c : public OwDev{
	private:
		// Assuming ad_fd and other globals are defined elsewhere
		uint8_t lastSeq = 0xff;
		int power = 0;
		int mode = 0;
		int power_total = 0;
		// timestamp of the last power-meter impulse, used to turn the
		// interval between impulses into an estimated wattage
		HrClock::time_point last_imp{};
		// running min/max/average of how long interrupt handling takes,
		// tracked across the lifetime of the daemon; exposed via fs_read
		std::chrono::milliseconds min_dur{std::chrono::milliseconds::max()};
		std::chrono::milliseconds max_dur{std::chrono::milliseconds::zero()};
		std::chrono::milliseconds sum_dur{std::chrono::milliseconds::zero()};
		uint32_t dur_count = 0;
		uint8_t read();
		void write(uint8_t cmd);
		void write_data(uint8_t cmd, uint8_t data);
		int read_buffer(uint8_t* buf, int len);
		// fs_table.h handlers
		int r_mode(char* buf, size_t size, bool uncached, int idx);
		int w_mode(const char* buf, size_t size, int idx);
		int r_power(char* buf, size_t size, bool uncached, int idx);
		int r_pow_total(char* buf, size_t size, bool uncached, int idx);
		int w_pow_total(const char* buf, size_t size, int idx);
		int w_test(const char* buf, size_t size, int idx);
		int r_int_min(char* buf, size_t size, bool uncached, int idx);
		int r_int_max(char* buf, size_t size, bool uncached, int idx);
		int r_int_avg(char* buf, size_t size, bool uncached, int idx);
		static const FsEntry<Ard_i2c> table[];
		static const size_t n_table;
#ifdef TESTING
		// GTest testing support
		FRIEND_TEST(FsTest, ArduinoPower);
#endif
	public:
		using OwDev::OwDev;
		using OwDev::type;
		Ard_i2c();
		Ard_i2c(std::string rom);
		json to_json() const override;
		virtual void from_json(const json& j);
		void set_mode(int mode);
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;

		void begin(bool soft=false) override;
		void end();
#if 0
		void events(int fd, OwDevices* ow);
#endif
		int interrupt();
};