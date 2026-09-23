#include <stdint.h>
#include "ow_devices.h"

int i2c_write(int fd, uint8_t cmd);
int i2c_write_data(int fd, uint8_t* buf, uint16_t size);
uint8_t i2c_read(int fd);
int i2c_read_data(int fd, uint8_t* buf, uint16_t size);

class Ard_i2c : public OwDev{
	private:
		// Assuming ad_fd and other globals are defined elsewhere
		uint8_t lastSeq;
		int power;
		int mode;
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
		void interrupt();
};