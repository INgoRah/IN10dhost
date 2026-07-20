#include <stdint.h>
#include "ow_devices.h"

int i2c_write(int fd, uint8_t cmd);
int i2c_write_data(int fd, uint8_t* buf, uint16_t size);
uint8_t i2c_read(int fd);
int i2c_read_data(int fd, uint8_t* buf, uint16_t size);

class Ard_i2c : public OwDev{
	private:
		OwDevices* ow;
		// Assuming ad_fd and other globals are defined elsewhere
		uint8_t lastSeq;
		int power;
		uint8_t read();
		void write(uint8_t cmd);
		void write_data(uint8_t cmd, uint8_t data);
		int read_buffer(uint8_t* buf, int len);
	public:
		using OwDev::OwDev;
		using OwDev::type;
		Ard_i2c();
		Ard_i2c(std::string rom);
		void set_mode(int mode) override;
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;

		int begin(OwDevices* ow);
		void end();
		void events(int fd);
		void interrupt();
};