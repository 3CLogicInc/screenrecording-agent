#pragma once

#include <string>
#include "../types.hpp"

namespace screenrec::fsm {
struct StartRecordingEvent {
    SessionId sessionId;
    RecordingConfig config;
    std::string outputPath;
};

struct PauseRecordingEvent {};

struct ResumeRecordingEvent {};

struct StopRecordingEvent {
    bool userInitiated = true;
};

struct AutoStopEvent {
    std::string reason = "timeout";
};

struct CaptureFailureEvent {
    std::string errorMessage;
};

struct FinalizeFailureEvent {
    std::string errorMessage;
};

struct FinalizeCompleteEvent {
    std::string filePath;
    uint64_t fileSize = 0;
};

struct CleanupEvent {
    bool preserveLogs = true;
};

struct DestroySessionEvent {};

}
