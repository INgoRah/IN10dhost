#pragma once

#include "ow_dev.h"
#include "interface/ds2450.h"

class ds2450 : public OwDev, public IDS2450 {
	private:
		uint16_t dat[4];
		float volt_a, volt_b, volt_c, volt_d;
		int fs_read_volt(uint8_t ch, char* buf, bool uncached);
	public:
		using OwDev::OwDev;
		using OwDev::type;
			ds2450() {
				mode = 0;
				volt_a = 0.0f;
				volt_b = 0.0f;
				volt_c = 0.0f;
				volt_d = 0.0f;
				type = "ds2450";
			};
			ds2450(std::string rom) : OwDev(rom) {
				mode = 0;
				volt_a = 0.0f;
				volt_b = 0.0f;
				volt_c = 0.0f;
				volt_d = 0.0f;
				type = "ds2450";
			}
		int poll();
		std::vector<string> fs_dir(string& path) const override;
		int fs_attr(string& path) const override;
		int fs_read(string& path, char* buf, size_t size, bool uncached) override;
		/// ds2450 specific functions
		int16_t adc_read(uint8_t ch, uint8_t flag);
		float adc_get(uint8_t ch);
};
