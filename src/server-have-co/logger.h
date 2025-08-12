#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>

enum class LogLevel { SILENT = 0, ERROR = 1, INFO = 2, DEBUG = 3 };

class Logger {
 public:
  static constexpr auto default_level_ = LogLevel::INFO;

  template <LogLevel Level>
  static void log(const std::string& message) {
    if constexpr (Level <= default_level_) {
      switch (Level) {
        case LogLevel::ERROR:
          std::cout << "[ERROR] " << message << std::endl;
          break;
        case LogLevel::INFO:
          std::cout << "[INFO] " << message << std::endl;
          break;
        case LogLevel::DEBUG:
          std::cout << "[DEBUG] " << message << std::endl;
          break;
        default:
          std::cout << message << std::endl;
          break;
      }
    }
  }

  static void error(const std::string& message) {
    log<LogLevel::ERROR>(message);
  }
  static void info(const std::string& message) { log<LogLevel::INFO>(message); }
  static void debug(const std::string& message) {
    log<LogLevel::DEBUG>(message);
  }
};

#endif  // LOGGER_H