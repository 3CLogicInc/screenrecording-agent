#pragma once

#include <mutex>
#include <string>
#include <memory>
#include "../types.hpp"

namespace spdlog {
class logger;
};
namespace screenrec {
class FileLogger {
public:
    explicit FileLogger(LoggingConfig config);
    ~FileLogger();

    bool initialize();
    void write(const std::string& message, LogLevel level);
    void flush();
    void setLevel(LogLevel level);

private:
    void openNewLogfile();
    void pruneOldLogs();
    std::string generateLogFileName() const;
    LoggingConfig config_;
    std::shared_ptr<spdlog::logger> logger_;
    std::string currentLogFile_;
    std::mutex rollMutex_;
    std::string lastBaseName_;
    size_t sameSecondRolls_{0};
    size_t bytesWritten_{0};
};

}
