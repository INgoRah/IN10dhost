#include <cctype>
#include <climits>
#include <stdexcept>
#include <string>
#include "parse.h"

int parse_num(const char* buf, size_t size)
{
	std::string s(buf, size);
	auto space = [](char c) {
		return c == '\0' || std::isspace((unsigned char)c);
	};
	size_t b = 0, e = s.size();

	while (b < e && space(s[b]))
		b++;
	while (e > b && space(s[e - 1]))
		e--;
	s = s.substr(b, e - b);

	size_t digits = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
	int base = (s.size() > digits + 1 && s[digits] == '0'
		&& (s[digits + 1] == 'x' || s[digits + 1] == 'X')) ? 16 : 10;
	size_t end;
	long v = std::stol(s, &end, base);

	if (end != s.size())
		throw std::invalid_argument("trailing characters");
	if (v < INT_MIN || v > INT_MAX)
		throw std::out_of_range("not an int");
	return (int)v;
}
