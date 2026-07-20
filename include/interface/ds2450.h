#pragma once

class IDS2450 {
	public:
		/** Starts conversion or reads the ADC value from the specified channel
		 * @param ch 0..3 (A - D)
		 * @param flag selects the operation
		 *   - 0: start conversion
		 *   - 1: read result
		 *   - 2: read cached value
		 */
		virtual int16_t adc_read(uint8_t ch, uint8_t flag) = 0;
};