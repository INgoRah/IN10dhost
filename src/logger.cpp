#include <algorithm>
#include <iostream>
#include <string>
#include <unistd.h> // for isatty
#include "logger.h"

void Logger::set_level(LogLevel level) {
	_level = level;
}

LogLevel Logger::get_level() {
	return _level;
}

#ifndef NO_TERM
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
#endif

Logger::Recent Logger::warnings() const
{
	std::lock_guard<std::mutex> lk(recent_mtx);
	return recent_warn;
}

Logger::Recent Logger::errors() const
{
	std::lock_guard<std::mutex> lk(recent_mtx);
	return recent_err;
}

void Logger::log(LogLevel level, const std::string& msg) {
#ifndef NO_TERM
	bool is_terminal = isatty(STDOUT_FILENO);
#endif
	if (level == LogLevel::WARN || level == LogLevel::ERROR) {
		// counted even when the level does not show them
		std::lock_guard<std::mutex> lk(recent_mtx);
		Recent& r = level == LogLevel::WARN ? recent_warn : recent_err;
		r.count++;
		r.last = msg;
		r.when = std::chrono::system_clock::now();
	}
	if (_level < level) {
		return; // Skip messages below the current log level
	}
#ifndef NO_TERM
	if (is_terminal) {
		// Foreground: Pretty-print with labels
		std::cout << "[" << levelToString(level) << "] " << msg << std::endl;
	} else {
#endif
		// Background: Use systemd journal prefixes
		// systemd parses "<N>" at the start of a line as priority N,
		// 0..7 only: VERBOSE (8) is logged as debug (7), an "<8>"
		// would end up in the message text at the default priority
		std::cout << "<" << std::min(static_cast<int>(level), 7) << ">" << msg << std::endl;
#ifndef NO_TERM
	}
#endif
}

