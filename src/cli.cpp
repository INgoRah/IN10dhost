#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include "cli.h"

Cli cli;

bool Cli::match_flag(const std::string& arg, const char* short_name, const char* long_name) const
{
	return (short_name && arg == short_name) || (long_name && arg == long_name);
}

// Matches argv[i] against a short/long option that takes a value, in
// any of "-x val", "-x=val", "--long val", "--long=val" form (either
// name may be nullptr to mean "no such form"). On a match, sets value
// and consumed (1 for the "=" form or a same-token flag with no value
// following, 2 for the separate-token form) and returns true; the
// missing-value case is left for the caller to report, since only it
// knows which option name to put in the message.
bool Cli::match_value_opt(int argc, char* argv[], int i, const char* short_name,
	const char* long_name, std::string& value, int& consumed) const
{
	std::string arg = argv[i];
	const char* name = nullptr;

	if (short_name && arg.compare(0, strlen(short_name), short_name) == 0)
		name = short_name;
	else if (long_name && arg.compare(0, strlen(long_name), long_name) == 0)
		name = long_name;
	else
		return false;

	size_t name_len = strlen(name);
	if (arg.size() > name_len && arg[name_len] == '=') {
		value = arg.substr(name_len + 1);
		consumed = 1;
		return true;
	}
	if (arg.size() != name_len)
		return false; // e.g. "--data-foo": not actually this option

	if (i + 1 >= argc) {
		value.clear();
		consumed = 1;
	} else {
		value = argv[i + 1];
		consumed = 2;
	}
	return true;
}

void Cli::parse(int& argc, char* argv[])
{
	for (int i = 1; i < argc; i++) {
		std::string value;
		int consumed = 0;

		if (match_flag(argv[i], "-h", "--help")) {
			help_requested = true;
			// Deliberately not consumed: leave it in argv so
			// fuse_main() also prints its own help for FUSE's options.
			continue;
		}

		if (match_value_opt(argc, argv, i, nullptr, "--gpio-pin", value, consumed)) {
			if (value.empty()) {
				fprintf(stderr, "--gpio-pin requires a value, keeping %d\n", gpio_pin);
			} else {
				try {
					int pin = std::stoi(value);
					if (pin < 0)
						throw std::out_of_range(value);
					gpio_pin = pin;
				} catch (const std::exception&) {
					fprintf(stderr, "Invalid --gpio-pin value '%s', keeping %d\n",
						value.c_str(), gpio_pin);
				}
			}
		} else if (match_flag(argv[i], "-s", "--soft")) {
			soft_start = true;
			consumed = 1;
		} else if (match_value_opt(argc, argv, i, "-d", "--data", value, consumed)) {
			if (value.empty())
				fprintf(stderr, "--data requires a value, keeping '%s'\n", data_path.c_str());
			else
				data_path = value;
		} else {
			continue;
		}

		// Remove the consumed argv entries, shifting the rest down, and
		// re-check the same index next (it now holds the following arg).
		for (int j = i; j + consumed < argc; j++)
			argv[j] = argv[j + consumed];
		argc -= consumed;
		i--;
	}
}

void Cli::print_help(const char* prog_name) const
{
	printf("%s options:\n", prog_name);
	printf("  --gpio-pin=<N>, --gpio-pin <N>   GPIO line for the Arduino interrupt\n");
	printf("                                   (default: %d; 0 deactivates GPIO)\n", GPIO_LINE_DEFAULT);
	printf("  -s, --soft                       Soft start (reserved, not yet acted on)\n");
	printf("  -d, --data <path>                Path to data.json\n");
	printf("                                   (default: <current directory>/data.json)\n");
	printf("  -h, --help                       Show this help, then FUSE's own help\n");
	printf("\n");
}
