#pragma once

#include <boost/sml.hpp>
#include "fsm_events.hpp"
#include "../recording/recording_context.hpp"
#include "../recording/recording_engine.hpp"
#include "../constants.hpp"
#include <memory>

namespace screenrec::fsm {
namespace sml = boost::sml;
struct Idle {};
struct Recording {};
struct Paused {};
struct Stopping {};
struct Stopped {};
struct Error {};

struct RecordingStateMachine {
    auto operator()() const noexcept {
        using sml::event;
        using sml::make_transition_table;
        using sml::state;

        auto onStartRecording = [](const StartRecordingEvent& event, RecordingContext& ctx) {
            ctx.setSessionId(event.sessionId);
            ctx.setConfig(event.config);

            std::string outputPath = event.outputPath;
            if (outputPath.empty()) {
                outputPath = constants::getRecordingFilePath(event.sessionId, event.config.outputFormat);
                ctx.logSessionWarning("Output path not provided in event, using default: " + outputPath);
            }

            ctx.logSessionInfo("Starting recording, output: " + outputPath);
            ctx.setFilePath(outputPath);
            auto engine = ctx.getEngine();
            if (!engine) {
                ctx.setError("Recording engine is null");
                ctx.updateState(RecordingState::ERR);
                return;
            }
            engine->setOutputPath(outputPath);
            if (engine->initialize(event.config) && engine->start()) {
                ctx.setStartTime(std::chrono::system_clock::now());
                ctx.updateState(RecordingState::RECORDING);
                ctx.logSessionInfo("Recording started successfully");
            } else {
                ctx.setError("Failed to start recording engine");
                ctx.updateState(RecordingState::ERR);
            }
        };

        auto onPauseRecording = [](RecordingContext& ctx) {
            const auto engine = ctx.getEngine();
            if (engine && engine->pause()) {
                ctx.updateState(RecordingState::PAUSED);
            } else {
                ctx.logSessionError("Failed to pause recording engine");
            }
        };

        auto onResumeRecording = [](RecordingContext& ctx) {
            const auto engine = ctx.getEngine();
            if (engine && engine->resume()) {
                ctx.updateState(RecordingState::RECORDING);
            } else {
                ctx.logSessionError("Failed to resume recording engine");
            }
        };

        auto onStopRecording = [](RecordingContext& ctx) {
            auto engine = ctx.getEngine();
            if (engine) {
                engine->stop();
            }
            ctx.setEndTime(std::chrono::system_clock::now());
            ctx.updateState(RecordingState::STOPPING);
            ctx.logSessionInfo("Recording stopping, finalizing file...");
        };

        auto onCaptureFailure = [](const CaptureFailureEvent& event, RecordingContext& ctx) {
            ctx.updateState(RecordingState::ERR);
            ctx.setError(event.errorMessage);
        };

        auto onFinalizeComplete = [](const FinalizeCompleteEvent& event, RecordingContext& ctx) {
            ctx.setFilePath(event.filePath);
            ctx.setFileSizeBytes(event.fileSize);
            ctx.updateState(RecordingState::STOPPED);
            ctx.logSessionInfo("Recording finalized, ready for upload: " + event.filePath);
        };

        auto onFinalizeFailure = [](const FinalizeFailureEvent& event, RecordingContext& ctx) {
            ctx.updateState(RecordingState::ERR);
            ctx.setError(event.errorMessage);
        };

        auto onCleanup = [](RecordingContext& ctx) {
            ctx.updateState(RecordingState::IDLE);
            ctx.logSessionInfo("Session cleaned up");
        };

        auto onDestroy = [](RecordingContext& ctx) {
            ctx.updateState(RecordingState::IDLE);
            ctx.logSessionInfo("Session destroyed");
        };

        // SML DSL: src + event<E> [guard] / action = dst; *state marks the initial state.
        return make_transition_table(
            *state<Idle> + event<StartRecordingEvent> / onStartRecording = state<Recording>,

            state<Recording> + event<PauseRecordingEvent> / onPauseRecording = state<Paused>,
            state<Recording> + event<StopRecordingEvent> / onStopRecording = state<Stopping>,
            state<Recording> + event<AutoStopEvent> / onStopRecording = state<Stopping>,
            state<Recording> + event<CaptureFailureEvent> / onCaptureFailure = state<Error>,

            state<Paused> + event<ResumeRecordingEvent> / onResumeRecording = state<Recording>,
            state<Paused> + event<StopRecordingEvent> / onStopRecording = state<Stopping>,
            state<Paused> + event<AutoStopEvent> / onStopRecording = state<Stopping>,
            state<Paused> + event<CaptureFailureEvent> / onCaptureFailure = state<Error>,

            state<Stopping> + event<FinalizeCompleteEvent> / onFinalizeComplete = state<Stopped>,
            state<Stopping> + event<FinalizeFailureEvent> / onFinalizeFailure = state<Error>,

            state<Stopped> + event<DestroySessionEvent> / onDestroy = state<Idle>,

            state<Error> + event<CleanupEvent> / onCleanup = state<Idle>);
    }
};

using RecordingSM = sml::sm<RecordingStateMachine>;

class RecordingFSM {
public:
    explicit RecordingFSM(RecordingContext& context)
        : sm_(std::make_unique<RecordingSM>(context)), context_(context) {
    }

    template <typename Event>
    void processEvent(const Event& event) {
        sm_->process_event(event);
    }

    // Read from RecordingContext, not the SML state.
    [[nodiscard]] bool isRecording() const { return context_.getState() == RecordingState::RECORDING; }
    [[nodiscard]] bool isPaused() const { return context_.getState() == RecordingState::PAUSED; }

private:
    std::unique_ptr<RecordingSM> sm_;
    RecordingContext& context_;
};

}
