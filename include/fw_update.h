#ifndef _FW_UPDATE_H
#define _FW_UPDATE_H
/* Firmware update of an owSlave over 1-Wire, through its bootloader.
 * The protocol is owSlave's include/owbootloader.h, the constants
 * below have to stay the same as there. */
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define BL_VERSION	2
#define BL_ENTER_CMD	0xB0
#define BL_ENTER_KEY	0x07
#define BL_CMD_INFO	0xB1
#define BL_CMD_WRITE	0xB2
#define BL_CMD_READ	0xB3
#define BL_CMD_EXIT	0xB4
#define BL_CONFIRM	0xAA
#define BL_OK		0xAA
#define BL_ERR_ADDR	0x01
#define BL_ERR_VERIFY	0x02
#define BL_ERR_NOAPP	0x03
/* the size of the BL_CMD_INFO answer, without its CRC */
#define BL_INFO_SIZE	11

struct BlInfo {
	uint8_t version = 0;
	uint16_t page_size = 0;
	// BL_START: the application may use the flash below it
	uint16_t app_size = 0;
	uint8_t signature[3] = {};
	bool app_valid = false;
	uint8_t last_result = 0;
};

/* The answer of BL_CMD_INFO, false if it is none */
bool bl_info_parse(const uint8_t* raw, BlInfo& info);

/* The device in its bootloader, one transaction per call. The ones
   returning int give 0 (write_page: BL_OK) or a negative errno. */
class FwLink {
	public:
		virtual ~FwLink() = default;
		virtual int info(BlInfo& info) = 0;
		// BL_OK, BL_ERR_* of the device, or a negative errno (no
		// answer, the CRC of the transfer wrong)
		virtual int write_page(uint16_t addr, const uint8_t* data, size_t len) = 0;
		virtual int read_page(uint16_t addr, uint8_t* data, size_t len) = 0;
		// to the application; -ENOEXEC if the device has none
		virtual int exit() = 0;
};

/* A firmware image: Intel HEX (.hex, as PlatformIO builds it) or the
   raw binary, from address 0. Gaps are 0xff. Returns 0, or a negative
   errno with err telling why. */
int fw_load(const std::string& path, std::vector<uint8_t>& image, std::string& err);
int fw_parse_hex(const std::string& text, std::vector<uint8_t>& image, std::string& err);

/* Writes image to the device, each page verified by reading it back.
   Page 0 (the reset vector) is erased first and written last: an
   update broken off leaves no application, the device stays in its
   bootloader and the update can be done again. Does not leave the
   bootloader. progress(done, total) after each page. Returns 0 or a
   negative errno, err telling why. */
int fw_flash(FwLink& link, const std::vector<uint8_t>& image,
	const std::function<void(int, int)>& progress, std::string& err);

#endif
