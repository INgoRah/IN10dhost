#include "ow_devices.h"
#include "interface/ds1820.h"
#include "fs_table.h"

#define DS1820_CFG_SIZE 8

class ds1820 : public OwDev, public IDS1820 {
	private:
		uint8_t scratchPad[9] = {};
		uint8_t cfg[DS1820_CFG_SIZE] = {};
		float temp = 21.5f;
		uint8_t hum = 50;
		// fs_table.h handlers
		int r_temperature(char* buf, size_t size, bool uncached, int idx);
		int r_humidity(char* buf, size_t size, bool uncached, int idx);
		int r_cfg(char* buf, size_t size, bool uncached, int idx);
		int w_cfg(const char* buf, size_t size, int idx);
		static const FsEntry<ds1820> table[];
		static const size_t n_table;

	public:
		using OwDev::OwDev;
		using OwDev::type;
		ds1820() { temp = 21.5f; hum = 50; type = "ds1820"; };
		ds1820(std::string rom) : OwDev(std::move(rom)) { temp = 21.5f; hum = 50;type = "ds1820"; };
		int poll();

		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;
		int fs_write(string& path, const char* buf, size_t size) override;

		// IDS1820
		float temp_read(const uint8_t flag);
		int cfg_read();
		int cfg_write(const uint8_t* data, int len = DS1820_CFG_SIZE);
};
