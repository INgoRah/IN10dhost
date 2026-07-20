/*
 Circular buffer library for Arduino.
 Copyright (c) 2017 Roberto Lo Giacco.

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
 */
#include "ring_buffer/ring_buffer.h"

template<typename T, size_t S, typename IT>
constexpr RingBuffer<T,S,IT>::RingBuffer() :
		head(buffer), tail(buffer), count(0) {
}

template<typename T, size_t S, typename IT>
bool RingBuffer<T,S,IT>::push(T value) {
	if (++tail == buffer + capacity) {
		tail = buffer;
	}
	*tail = value;
	if (count == capacity) {
		if (++head == buffer + capacity) {
			head = buffer;
		}
		return false;
	} else {
		if (count++ == 0) {
			head = tail;
		}
		return true;
	}
}

template<typename T, size_t S, typename IT>
T RingBuffer<T,S,IT>::pop() {
	if (count == 0) return *tail;
	T result = *tail--;
	if (tail < buffer) {
		tail = buffer + capacity - 1;
	}
	count--;
	return result;
}

template<typename T, size_t S, typename IT>
T RingBuffer<T,S,IT>::operator [](IT index) const {
	if (index >= count) return *tail;
	return *(buffer + ((head - buffer + index) % capacity));
}

template<typename T, size_t S, typename IT>
IT inline RingBuffer<T,S,IT>::size() const {
	return count;
}

template<typename T, size_t S, typename IT>
void inline RingBuffer<T,S,IT>::clear() {
	head = tail = buffer;
	count = 0;
}
