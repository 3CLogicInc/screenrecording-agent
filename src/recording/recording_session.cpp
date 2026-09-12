#include "recording/recording_session.hpp"
#include "recording/recording_engine_libobs.hpp"
#include "recording/recording_context.hpp"
#include "recording/pause_transition.hpp"
#include "storage/file_manager.hpp"
#include "logging/log_service.hpp"
#include "fsm/fsm_events.hpp"
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <utility>

namespace screenrec {

RecordingSession::RecordingSession(
    SessionId sessionId,
    std::shared_ptr<RecordingContext> context) : sessionId_(std::move(sessionId)),
                                                 context_(std::move(context)) {
    fsm_ = std::make_unique<fsm::RecordingFSM>(*context_);
}

RecordingSession::~RecordingSession() {
    if (!destroyed_) {
        stop();
    }
}

void RecordingSession::ensureNotDestroyed() const {
    if (destroyed_) {
        throw std::runtime_error("Session has been destroyed");
    }
}

bool RecordingSession::start(const RecordingConfig& config) {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    context_->setConfig(config);
    context_->setSessionId(sessionId_);

    auto engine = context_->getEngine();
    if (!engine) {
        context_->setError("Recording engine not available");
        context_->updateState(RecordingState::ERR);
        return false;
    }

    auto fileManager = context_->getFileManager();
    if (!fileManager) {
        context_->setError("File manager not available");
        context_->updateState(RecordingState::ERR);
        return false;
    }

    std::string outputPath = fileManager->createOutputFile(sessionId_, config.outputFormat);
    if (context_->getLogger()) {
        context_->getLogger()->info("RecordingSession", "Recording output will be saved to: " + outputPath);
    }

    fsm::StartRecordingEvent event;
    event.sessionId = sessionId_;
    event.config = config;
    event.outputPath = outputPath;
    fsm_->processEvent(event);

    return context_->getState() == RecordingState::RECORDING;
}

bool RecordingSession::pause() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const auto state = context_->getState();
    if (state == RecordingState::PAUSED) {
        return true;
    }
    if (state != RecordingState::RECORDING) {
        return false;
    }

    fsm_->processEvent(fsm::PauseRecordingEvent{});
    return context_->getState() == RecordingState::PAUSED;
}

bool RecordingSession::resume() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const auto state = context_->getState();
    if (state == RecordingState::RECORDING) {
        return true;
    }
    if (state != RecordingState::PAUSED) {
        return false;
    }

    fsm_->processEvent(fsm::ResumeRecordingEvent{});
    return context_->getState() == RecordingState::RECORDING;
}

bool RecordingSession::stop() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    auto currentState = context_->getState();
    if (currentState != RecordingState::RECORDING &&
        currentState != RecordingState::PAUSED) {
        return false;
    }

    fsm::StopRecordingEvent event;
    event.userInitiated = true;
    fsm_->processEvent(event);

    std::string filePath = context_->getFilePath();
    uint64_t fileSize = 0;
    if (!filePath.empty()) {
        try {
            fileSize = std::filesystem::file_size(filePath);
            context_->setFileSizeBytes(fileSize);
        } catch (const std::filesystem::filesystem_error&) {
            // File might not exist yet or is still being written
            context_->setFileSizeBytes(0);
        }
    }

    // engine->stop() is synchronous, so finalize can fire immediately.
    if (context_->getState() == RecordingState::STOPPING && !filePath.empty()) {
        fsm::FinalizeCompleteEvent finalizeEvent;
        finalizeEvent.filePath = filePath;
        finalizeEvent.fileSize = fileSize;
        fsm_->processEvent(finalizeEvent);
    }

    return context_->getState() == RecordingState::STOPPED ||
           context_->getState() == RecordingState::STOPPING;
}

RecordingState RecordingSession::getState() const {
    std::scoped_lock lock(mutex_);
    return context_->getState();
}

RecordingMetadata RecordingSession::getMetadata() const {
    std::scoped_lock lock(mutex_);
    return context_->getMetadata();
}

void RecordingSession::handleCaptureFailure(const std::string& error) {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();
    context_->setError(error);

    fsm::CaptureFailureEvent event;
    event.errorMessage = error;
    fsm_->processEvent(event);
}

void RecordingSession::destroy() {
    std::scoped_lock lock(mutex_);
    if (!destroyed_) {
        fsm_->processEvent(fsm::DestroySessionEvent{});
        destroyed_ = true;
    }
}

bool RecordingSession::pauseAudio() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const PauseTransition t = computePauseTransition(context_->getPauseState(),
        PauseStream::AUDIO, /*pausing=*/true);
    if (t.alreadyInDesiredState) {
        return true;
    }
    if (context_->getState() != RecordingState::RECORDING) {
        return false;
    }

    if (context_->getEngine() && context_->getEngine()->pauseAudio()) {
        context_->setPauseState(t.next);
        return true;
    }

    return false;
}

bool RecordingSession::pauseVideo() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const PauseTransition t = computePauseTransition(context_->getPauseState(),
        PauseStream::VIDEO, /*pausing=*/true);
    if (t.alreadyInDesiredState) {
        return true;
    }
    if (context_->getState() != RecordingState::RECORDING) {
        return false;
    }

    if (context_->getEngine() && context_->getEngine()->pauseVideo()) {
        context_->setPauseState(t.next);
        return true;
    }

    return false;
}

bool RecordingSession::resumeAudio() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const PauseTransition t = computePauseTransition(context_->getPauseState(),
        PauseStream::AUDIO, /*pausing=*/false);
    if (t.alreadyInDesiredState) {
        return true;
    }

    if (context_->getEngine() && context_->getEngine()->resumeAudio()) {
        context_->setPauseState(t.next);
        return true;
    }

    return false;
}

bool RecordingSession::resumeVideo() {
    std::scoped_lock lock(mutex_);
    ensureNotDestroyed();

    const PauseTransition t = computePauseTransition(context_->getPauseState(),
        PauseStream::VIDEO, /*pausing=*/false);
    if (t.alreadyInDesiredState) {
        return true;
    }

    if (context_->getEngine() && context_->getEngine()->resumeVideo()) {
        context_->setPauseState(t.next);
        return true;
    }

    return false;
}

}
