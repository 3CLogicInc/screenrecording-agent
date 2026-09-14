#include "logging/file_logger.hpp"
#include "logging/log_service.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <utility>
#undef ERROR
namespace fs = std::filesystem;

namespace screenrec {
static spdlog::level::level_enum toSpdLevel(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return spdlog::level::debug;
    case LogLevel::INFO: return spdlog::level::info;
    case LogLevel::WARNING: return spdlog::level::warn;
    case LogLevel::ERR: return spdlog::level::err;
    default: return spdlog::level::critical;
    }
}
FileLogger::FileLogger(LoggingConfig config) : config_(std::move(config)) {}

FileLogger::~FileLogger() {
    std::scoped_lock lock(rollMutex_);
    if (logger_) {
        logger_->flush();
        spdlog::drop(logger_->name());
    }
}

bool FileLogger::initialize() {
    try {
        fs::create_directories(config_.logDirectory);
        openNewLogfile();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void FileLogger::write(const std::string& message, LogLevel level) {
    std::scoped_lock lock(rollMutex_);
    if (!logger_) return;
    logger_->log(toSpdLevel(level), message);
    logger_->flush();
    // "[2026-01-01 00:00:00.000] [warning] " + newline
    constexpr size_t kLineOverheadBytes = 38;
    bytesWritten_ += message.size() + kLineOverheadBytes;
    if (bytesWritten_ >= config_.maxFileSizeBytes) {
        openNewLogfile();
    }
}

void FileLogger::flush() {
    std::scoped_lock lock(rollMutex_);
    if (logger_) {
        logger_->flush();
    }
}

void FileLogger::setLevel(LogLevel level) {
    std::scoped_lock lock(rollMutex_);
    config_.level = level;
    if (logger_) {
        logger_->set_level(toSpdLevel(level));
        logger_->flush_on(toSpdLevel(level));
    }
}
// Rolls to a NEW file at the size cap; truncating in place would lose data and desync the shipper.
void FileLogger::openNewLogfile() {
    if (logger_) {
        logger_->flush();
        spdlog::drop(logger_->name());
        logger_ = nullptr;
    }
    std::string name = generateLogFileName();
    // Same-second rolls regenerate the same name; the suffix keeps each roll on a fresh file.
    if (name == lastBaseName_) {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "_r%03zu", ++sameSecondRolls_);
        name.insert(name.size() - 4, suffix);
    } else {
        lastBaseName_ = name;
        sameSecondRolls_ = 0;
    }
    currentLogFile_ = name;
    logger_ = spdlog::basic_logger_mt("file_logger_" + currentLogFile_, currentLogFile_);
    logger_->set_level(toSpdLevel(config_.level));
    logger_->flush_on(toSpdLevel(config_.level));
    logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    bytesWritten_ = 0;
    pruneOldLogs();
}

void FileLogger::pruneOldLogs() {
    const std::string prefix = "screen_recording_" +
                               (config_.processTag.empty() ? std::string() : config_.processTag + "_");
    std::vector<fs::path> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(config_.logDirectory, ec)) {
        const std::string base = entry.path().filename().string();
        if (base.rfind(prefix, 0) == 0 && entry.path().extension() == ".log" &&
            entry.path().string() != currentLogFile_) {
            logs.push_back(entry.path());
        }
    }
    if (logs.size() + 1 <= config_.maxFiles) {
        return;
    }
    // Timestamped names sort chronologically.
    std::sort(logs.begin(), logs.end());
    const size_t excess = logs.size() + 1 - config_.maxFiles;
    for (size_t i = 0; i < excess; ++i) {
        fs::remove(logs[i], ec);
    }
}
std::string FileLogger::generateLogFileName() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

#ifdef _WIN32
    DWORD processId = GetCurrentProcessId();
#else
    pid_t processId = getpid();
#endif

    // Keep the "screen_recording_" prefix: pruneOldLogs() and the shipper's label mapping key on it.
    std::stringstream ss;
    ss << config_.logDirectory
       << "/screen_recording_";
    if (!config_.processTag.empty()) {
        ss << config_.processTag << "_";
    }
    ss << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S")
       << "_pid" << processId
       << ".log";

    return ss.str();
}

}
