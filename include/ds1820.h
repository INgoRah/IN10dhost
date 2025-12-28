#include "ow_devices.h"

class ds1820 : public OwDev {
	private:
		float temp;
		uint8_t hum;

	public:
		using OwDev::OwDev;
		using OwDev::type;
		ds1820() { this->temp = 21.5f; };
		ds1820(std::string rom) : OwDev(rom) { type = "ds1820"; };
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached = false) override;

		json to_json() const override {
			return json{
				{"type", type},
				{"bus", bus},
				{"rom", rom},
				{"id", id},
				{"name", name},
			};
		};

		int temp_read(const uint8_t mode);
};
