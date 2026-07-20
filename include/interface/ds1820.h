#ifndef _IDS1820_H
#define _IDS1820_H

class IDS1820 {
	public:
		virtual float temp_read(const uint8_t flag) = 0;
};

#endif // _IDS1820_H