#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include "../types.hpp"

namespace screenrec {

class IRecordingEngine;
class FileManager;
class LogService;

class RecordingContext {
public:
    RecordingContext(
        std::shared_ptr<IRecordingEngine> engine,
        std::shared_ptr<FileManager> fileManager,
        std::shared_ptr<LogService> logger);

    ~RecordingContext();

    void setSessionId(const SessionId& id) { sessionId_ = id; }
    SessionId getSessionId() const { return sessionId_; }

    void setConfig(const RecordingConfig& config) { config_ = config; }
    RecordingConfig getConfig() const { return config_; }

    RecordingMetadata getMetadata() const;

    void updateState(RecordingState state);
    RecordingState getState() const { return currentState_.load(); }

    void setError(const std::string& error);
    std::string getError() const;

    std::shared_ptr<IRecordingEngine> getEngine() { return engine_; }
    std::shared_ptr<FileManager> getFileManager() { return fileManager_; }
    std::shared_ptr<LogService> getLogger() { return logger_; }

    void setFilePath(const std::string& path) {
        std::scoped_lock lock(mutex_);
        filePath_ = path;
    }
    std::string getFilePath() const {
        std::scoped_lock lock(mutex_);
        return filePath_;
    }

    void setStartTime(const Timestamp& time) { startTime_ = time; }
    void setEndTime(const Timestamp& time) { endTime_ = time; }

    uint64_t getFileSizeBytes() const {
        std::scoped_lock lock(mutex_);
        return fileSizeBytes_;
    }
    void setFileSizeBytes(uint64_t size) {
        std::scoped_lock lock(mutex_);
        fileSizeBytes_ = size;
    }

    void setPauseState(PauseState state) {
        std::scoped_lock lock(mutex_);
        pauseState_ = state;
    }
    PauseState getPauseState() const {
        std::scoped_lock lock(mutex_);
        return pauseState_;
    }

    void logSessionInfo(const std::string& message) const;
    void logSessionError(const std::string& message) const;
    void logSessionWarning(const std::string& message) const;

    static std::string stateToString(RecordingState state);

private:
    SessionId sessionId_;
    RecordingConfig config_;
    std::atomic<RecordingState> currentState_{RecordingState::IDLE};

    std::shared_ptr<IRecordingEngine> engine_;
    std::shared_ptr<FileManager> fileManager_;
    std::shared_ptr<LogService> logger_;

    mutable std::mutex mutex_;
    std::string errorMessage_;
    std::string filePath_;

    Timestamp startTime_;
    Timestamp endTime_;
    uint64_t fileSizeBytes_ = 0;

    PauseState pauseState_ = PauseState::NONE;
};

}
