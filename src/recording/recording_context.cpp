#include "recording/recording_context.hpp"
#include "recording/recording_engine.hpp"
#include "storage/file_manager.hpp"
#include "logging/log_service.hpp"

#include <sstream>
#include <utility>

namespace screenrec {

std::string RecordingContext::stateToString(RecordingState state) {
    switch (state) {
    case RecordingState::IDLE: return "Idle";
    case RecordingState::RECORDING: return "Recording";
    case RecordingState::PAUSED: return "Paused";
    case RecordingState::STOPPING: return "Stopping";
    case RecordingState::STOPPED: return "Stopped";
    case RecordingState::ERR: return "Error";
    default: return "Unknown(" + std::to_string(static_cast<int>(state)) + ")";
    }
}

RecordingContext::RecordingContext(
    std::shared_ptr<IRecordingEngine> engine,
    std::shared_ptr<FileManager> fileManager,
    std::shared_ptr<LogService> logger) : engine_(std::move(engine)),
                                          fileManager_(std::move(fileManager)),
                                          logger_(std::move(logger)) {
    if (!logger_) {
        logger_ = LogService::noop();
    }
}

RecordingContext::~RecordingContext() = default;

RecordingMetadata RecordingContext::getMetadata() const {
    std::scoped_lock lock(mutex_);

    RecordingMetadata metadata;
    metadata.sessionId = sessionId_;
    metadata.startTime = startTime_;
    metadata.endTime = endTime_;
    metadata.duration = std::chrono::duration_cast<Duration>(endTime_ - startTime_);
    metadata.videoConfig = config_.video;
    metadata.audioConfig = config_.audio;
    metadata.filePath = filePath_;
    metadata.fileSizeBytes = fileSizeBytes_;
    metadata.state = currentState_.load();
    metadata.errorMessage = errorMessage_;

    return metadata;
}

void RecordingContext::updateState(RecordingState state) {
    RecordingState oldState = currentState_.exchange(state);
    if (logger_ && oldState != state) {
        std::stringstream ss;
        ss << "State transition: " << stateToString(oldState) << " -> " << stateToString(state);
        logger_->info("Context", ss.str(), sessionId_);
    }
}

void RecordingContext::setError(const std::string& error) {
    std::scoped_lock lock(mutex_);
    errorMessage_ = error;
    logger_->error("Context", "Error set: " + error, sessionId_);
}

std::string RecordingContext::getError() const {
    std::scoped_lock lock(mutex_);
    return errorMessage_;
}

void RecordingContext::logSessionInfo(const std::string& message) const {
    logger_->info("Context", message, sessionId_);
}

void RecordingContext::logSessionError(const std::string& message) const {
    logger_->error("Context", message, sessionId_);
}

void RecordingContext::logSessionWarning(const std::string& message) const {
    logger_->warning("Context", message, sessionId_);
}

}
