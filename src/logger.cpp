#include <iostream>
#include <string>
#include <unistd.h> // for isatty
#include "logger.h"

static LogLevel _level = LogLevel::INFO;

void Logger::set_level(LogLevel level) {
	_level = level;
}

LogLevel Logger::get_level() {
	return _level;
}

std::string Logger::levelToString(LogLevel level) {
	switch (level) {
		case LogLevel::DEBUG: return "DEBUG";
		case LogLevel::VERBOSE: return "VERBOSE";
		case LogLevel::INFO:  return "INFO";
		case LogLevel::WARN:  return "WARN";
		case LogLevel::ERROR: return "ERROR";
		default: return "UNKNOWN";
	}
}

void Logger::log(LogLevel level, const std::string& msg) {
	bool is_terminal = isatty(STDOUT_FILENO);

	if (_level < level) {
		return; // Skip messages below the current log level
	}
	if (is_terminal) {
		// Foreground: Pretty-print with labels
		std::cout << "[" << levelToString(level) << "] " << msg << std::endl;
	} else {
		// Background: Use systemd journal prefixes
		// systemd parses "<N>" at the start of a line as priority N
		std::cout << "<" << static_cast<int>(level) << ">" << msg << std::endl;
	}
}

