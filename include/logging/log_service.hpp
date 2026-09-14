#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <map>
#include "../types.hpp"

namespace screenrec {

class FileLogger;

struct LogEntry {
    LogLevel level;
    Timestamp timestamp;
    std::string sessionId;
    std::string component;
    std::string message;
    std::map<std::string, std::string> metadata;

    LogEntry(LogLevel lvl, std::string sess, std::string comp, std::string msg)
        : level(lvl), timestamp(std::chrono::system_clock::now()),
          sessionId(std::move(sess)), component(std::move(comp)), message(std::move(msg)) {}
};

class LogService {
public:
    explicit LogService(LoggingConfig config);
    ~LogService();

    // Sink-less shared instance (never initialized) so call sites need no logger_ null checks.
    static const std::shared_ptr<LogService>& noop();

    bool initialize();
    void shutdown();

    void debug(const std::string& component, const std::string& message,
        const std::string& sessionId = "");
    void info(const std::string& component, const std::string& message,
        const std::string& sessionId = "");
    void warning(const std::string& component, const std::string& message,
        const std::string& sessionId = "");
    void error(const std::string& component, const std::string& message,
        const std::string& sessionId = "");
    void critical(const std::string& component, const std::string& message,
        const std::string& sessionId = "");

    void log(const LogEntry& entry);
    void log(LogLevel level, const std::string& component, const std::string& message,
        const std::string& sessionId = "");

    void setLevel(LogLevel level);
    void setSessionContext(const std::string& sessionId);
    void clearSessionContext();
    LogLevel getLevel() const { return config_.level; }

    void setConfig(const LoggingConfig& config);
    LoggingConfig getConfig() const { return config_; }

    void flush();

private:
    bool shouldLog(LogLevel level) const;
    void writeLog(const LogEntry& entry);

    LoggingConfig config_;
    std::unique_ptr<FileLogger> fileLogger_;
    mutable std::mutex mutex_;
    std::string sessionContext_;
};

}
