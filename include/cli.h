#pragma once

#include <string>

// Pure, hardware-independent command-line handling, split out of main.cpp
// so it's reachable from both the daemon and the GTest binary (main.cpp
// itself is swapped out for test/main.cpp in TESTING builds).

// Default GPIO line for the Arduino interrupt, used unless overridden.
#define GPIO_LINE_DEFAULT 6

class Cli {
public:
	// Runtime GPIO line number. 0 means "GPIO deactivated" - checked by
	// setup_gpio()/background_worker() in main.cpp.
	int gpio_pin = GPIO_LINE_DEFAULT;

	// Set by -s/--soft. Reserved: not yet acted on anywhere, just parsed
	// and stored so a soft-start behavior can be wired up later without
	// another round of argv-handling plumbing.
	bool soft_start = false;

	// Set by -d/--data <path> or -d/--data=<path>. Empty means "not
	// given on the command line" - main.cpp falls back to its own
	// default (<cwd>/data.json) in that case; parse() never invents
	// one, since "current directory" is a property of the caller, not
	// of argv.
	std::string data_path;

	// Set by -h/--help. parse() only records this; it's up to the
	// caller to call print_help() and decide what to do next (main.cpp
	// skips straight to fuse_main(), whose own -h handling still fires
	// since parse() deliberately leaves -h/--help in argv - see below).
	bool help_requested = false;

	// Recognizes and strips this daemon's own options out of argv
	// before the rest is handed to fuse_main(), which only understands
	// its own options and a mount point. Must run before
	// setup_gpio()/fuse_main().
	//
	//   --gpio-pin=<N> | --gpio-pin <N>   GPIO line for the Arduino
	//                                     interrupt; 0 deactivates GPIO.
	//                                     Invalid/negative values are
	//                                     rejected with a warning,
	//                                     keeping the current value.
	//                                     Default: GPIO_LINE_DEFAULT.
	//   -s | --soft                      Soft start (see soft_start).
	//   -d <path> | --data <path> |
	//   -d=<path> | --data=<path>        Path to data.json (see
	//                                     data_path above).
	//   -h | --help                      Sets help_requested; left in
	//                                     argv (not consumed) so
	//                                     fuse_main() also prints its
	//                                     own help for FUSE's options.
	void parse(int& argc, char* argv[]);

	// Prints usage text for the options above to stdout.
	void print_help(const char* prog_name = "in10dfs") const;

private:
	bool match_flag(const std::string& arg, const char* short_name, const char* long_name) const;
	bool match_value_opt(int argc, char* argv[], int i, const char* short_name,
		const char* long_name, std::string& value, int& consumed) const;
};

// Shared, process-wide instance: parsed once in main(), read from
// setup_gpio()/background_worker(), and reset between cases by tests.
extern Cli cli;
