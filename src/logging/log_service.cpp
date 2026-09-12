#include "logging/log_service.hpp"
#include "logging/file_logger.hpp"

namespace screenrec {

LogService::LogService(LoggingConfig config)
    : config_(std::move(config)) {
}

const std::shared_ptr<LogService>& LogService::noop() {
    static const auto INSTANCE = [] {
        LoggingConfig cfg;
        cfg.enableFileLogging = false;
        return std::make_shared<LogService>(cfg);
    }();
    return INSTANCE;
}

LogService::~LogService() {
    shutdown();
}

bool LogService::initialize() {
    if (config_.enableFileLogging) {
        fileLogger_ = std::make_unique<FileLogger>(config_);
        if (!fileLogger_->initialize()) {
            return false;
        }
    }

    return true;
}

void LogService::shutdown() {
    flush();

    if (fileLogger_) {
        fileLogger_.reset();
    }
}

void LogService::debug(const std::string& component, const std::string& message,
    const std::string& sessionId) {
    log(LogLevel::DEBUG, component, message, sessionId);
}

void LogService::info(const std::string& component, const std::string& message,
    const std::string& sessionId) {
    log(LogLevel::INFO, component, message, sessionId);
}

void LogService::warning(const std::string& component, const std::string& message,
    const std::string& sessionId) {
    log(LogLevel::WARNING, component, message, sessionId);
}

void LogService::error(const std::string& component, const std::string& message,
    const std::string& sessionId) {
    log(LogLevel::ERR, component, message, sessionId);
}

void LogService::critical(const std::string& component, const std::string& message,
    const std::string& sessionId) {
    log(LogLevel::CRITICAL, component, message, sessionId);
}

void LogService::log(const LogEntry& entry) {
    if (!shouldLog(entry.level)) {
        return;
    }

    writeLog(entry);
}

void LogService::log(LogLevel level, const std::string& component, const std::string& message,
    const std::string& sessionId) {
    LogEntry entry(level, sessionId, component, message);
    {
        std::scoped_lock lock(mutex_);
        if (entry.level < config_.level) {
            return;
        }
        if (entry.sessionId.empty()) {
            entry.sessionId = sessionContext_;
        }
    }
    writeLog(entry);
}

// Attributes lines to the live session when the call site has none (OBS relay, engine internals).
void LogService::setSessionContext(const std::string& sessionId) {
    std::scoped_lock lock(mutex_);
    sessionContext_ = sessionId;
}

void LogService::clearSessionContext() {
    std::scoped_lock lock(mutex_);
    sessionContext_.clear();
}

void LogService::setLevel(LogLevel level) {
    {
        std::scoped_lock lock(mutex_);
        config_.level = level;
    }
    if (fileLogger_) {
        fileLogger_->setLevel(level);
    }
}

void LogService::setConfig(const LoggingConfig& config) {
    {
        std::scoped_lock lock(mutex_);
        config_ = config;
    }
    if (fileLogger_) {
        fileLogger_->setLevel(config.level);
    }
}

void LogService::flush() {
    if (fileLogger_) {
        fileLogger_->flush();
    }
}

bool LogService::shouldLog(LogLevel level) const {
    std::scoped_lock lock(mutex_);
    return level >= config_.level;
}

void LogService::writeLog(const LogEntry& entry) {
    if (fileLogger_) {
        std::string line;
        line.reserve(entry.component.size() + entry.sessionId.size() + entry.message.size() + 10);
        line += '[';
        line += entry.component;
        line += "] ";
        if (!entry.sessionId.empty()) {
            line += "[sid:";
            line += entry.sessionId;
            line += "] ";
        }
        line += entry.message;
        fileLogger_->write(line, entry.level);
    }
}

}
