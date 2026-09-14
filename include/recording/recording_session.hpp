#pragma once

#include <memory>
#include <string>
#include <mutex>
#include "../types.hpp"
#include "../fsm/recording_fsm.hpp"
#include "recording_context.hpp"

namespace screenrec {

class RecordingSession {
public:
    RecordingSession(
        SessionId sessionId,
        std::shared_ptr<RecordingContext> context);

    ~RecordingSession();

    bool start(const RecordingConfig& config);
    bool pause();
    bool resume();
    bool stop();

    bool pauseAudio();
    bool pauseVideo();
    bool resumeAudio();
    bool resumeVideo();

    SessionId getSessionId() const { return sessionId_; }
    RecordingState getState() const;
    RecordingMetadata getMetadata() const;

    void handleCaptureFailure(const std::string& error);

    void destroy();

private:
    SessionId sessionId_;
    std::shared_ptr<RecordingContext> context_;
    std::unique_ptr<fsm::RecordingFSM> fsm_;
    mutable std::mutex mutex_;
    bool destroyed_{false};

    void ensureNotDestroyed() const;
};

}
