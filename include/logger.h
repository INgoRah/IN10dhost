#ifndef _LOGGER_H
#define _LOGGER_H

#include <string>

enum class LogLevel {
    VERBOSE = 8,
    DEBUG = 7,
    INFO = 6,
    WARN = 4,
    ERROR = 3,
    NONE = 0
};

class ILogger {
public:
    virtual ~ILogger() {}
    virtual LogLevel get_level() = 0; // Pure virtual
    virtual void info(const std::string& msg) = 0;
    virtual void debug(const std::string& msg) = 0;
    virtual void verbose(const std::string& msg) = 0;
    virtual void error(const std::string& msg) = 0;
    virtual void warn(const std::string& msg) = 0;
};

#define STATIC
class Logger : public ILogger {
public:
	STATIC void set_level(LogLevel level);
    STATIC void log(LogLevel level, const std::string& msg);
	STATIC LogLevel get_level();
    STATIC void verbose(const std::string& msg) { log(LogLevel::VERBOSE, msg);}
    STATIC void debug(const std::string& msg) { log(LogLevel::DEBUG, msg);}
    STATIC void info(const std::string& msg) { log(LogLevel::INFO, msg);}
    STATIC void warn(const std::string& msg) { log(LogLevel::WARN, msg);}
    STATIC void error(const std::string& msg) { log(LogLevel::ERROR, msg);}

private:
    LogLevel _level = LogLevel::INFO;
    STATIC std::string levelToString(LogLevel level);
};

#endif
