/*
 Circular buffer library for Arduino.
 Copyright (c) 2017 Roberto Lo Giacco.
 Copyright (c) 2026 INgo Rah.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Lesser General Public License as 
 published by the Free Software Foundation, either version 3 of the 
 License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 Stripped to minimal functionality and renamed by INgo Rah.
 */
#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_
#include <stdlib.h>
#include <stdint.h>

namespace Helper {
	/** @private */
	template<bool FITS8, bool FITS16> struct Index {
		using Type = uint32_t;
	};

	/** @private */
	template<> struct Index<false, true> {
		using Type = uint16_t;
	};

	/** @private */
	template<> struct Index<true, true> {
		using Type = uint8_t;
	};
}

/**
 * @brief Implements a circular buffer that supports LIFO and FIFO operations.
 *
 * @tparam T The type of the data to store in the buffer.
 * @tparam S The maximum number of elements that can be stored in the buffer.
 * @tparam IT The data type of the index. Typically should be left as default.
 */
template<typename T, size_t S, typename IT = typename Helper::Index<(S <= UINT8_MAX), (S <= UINT16_MAX)>::Type> class RingBuffer {
public:
	/**
	 * @brief The buffer capacity.
	 *
	 * Read only as it cannot ever change.
	 */
	static constexpr IT capacity = static_cast<IT>(S);

	/**
	 * @brief Aliases the index type.
	 *
	 * Can be used to obtain the right index type with `decltype(buffer)::index_t`.
	 */
	using index_t = IT;

	/**
	 * @brief Create an empty circular buffer.
	 */
	constexpr RingBuffer();

	// disable the copy constructor
	/** @private */
	RingBuffer(const RingBuffer&) = delete;
	/** @private */
	RingBuffer(RingBuffer&&) = delete;

	// disable the assignment operator
	/** @private */
	RingBuffer& operator=(const RingBuffer&) = delete;
	/** @private */
	RingBuffer& operator=(RingBuffer&&) = delete;

	/**
	 * @brief Adds an element to the end of buffer.
	 *
	 * @return `false` iff the addition caused overwriting to an existing element.
	 */
	bool push(T value);

	/**
	 * @brief Removes an element from the end of the buffer.
	 *
	 * @warning Calling this operation on an empty buffer has an unpredictable behaviour.
	 */
	T pop();

	/**
	 * @brief Array-like access to buffer.
	 *
	 * Calling this operation using and index value greater than `size - 1` returns the tail element.
	 *
	 * @warning Calling this operation on an empty buffer has an unpredictable behaviour.
	 */
	T operator [] (IT index) const;

	/**
	 * @brief Returns how many elements are actually stored in the buffer.
	 *
	 * @return The number of elements stored in the buffer.
	 */
	IT inline size() const;

	/**
	 * @brief Resets the buffer to a clean status, making all buffer positions available.
	 *
	 * @note This does not clean up any dynamically allocated memory stored in the buffer.
	 * Clearing a buffer that points to heap-allocated memory may cause a memory leak, if it's not properly cleaned up.
	 */
	void inline clear();

private:
	T buffer[S];
	T *head;
	T *tail;
#ifndef RING_BUFFER_INT_SAFE
	IT count;
#else
	volatile IT count;
#endif
};

#endif
