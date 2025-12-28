#ifndef _LOGGER_H
#define _LOGGER_H

#include <chrono>

enum class LogLevel {
    VERBOSE = 8,
    DEBUG = 7,
    INFO = 6,
    WARN = 4,
    ERROR = 3
};

class Logger {
public:
	static void set_level(LogLevel level);
    static void log(LogLevel level, const std::string& msg);
	static LogLevel get_level();
    static void verbose(const std::string& msg) { log(LogLevel::VERBOSE, msg);}
    static void debug(const std::string& msg) { log(LogLevel::DEBUG, msg);}
    static void info(const std::string& msg) { log(LogLevel::INFO, msg);}
    static void warn(const std::string& msg) { log(LogLevel::WARN, msg);}
    static void error(const std::string& msg) { log(LogLevel::ERROR, msg);}

private:
    static std::string levelToString(LogLevel level);
};

#endif
