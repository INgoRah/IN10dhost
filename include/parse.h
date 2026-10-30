#ifndef _PARSE_H
#define _PARSE_H

#include <cstddef>

/* A number written to a leaf: decimal, or hex with 0x/0X ("0xFE"), with
   white space (echo's newline) or a terminating NUL around it at most.
   No octal, "010" is 10. Throws std::invalid_argument and
   std::out_of_range as std::stoi() does, also for anything after the
   number ("12abc"). The buffer need not be NUL terminated. */
int parse_num(const char* buf, size_t size);
#endif
