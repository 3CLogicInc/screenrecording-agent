#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <memory>
#include <string>
#include <vector>

#include "fsm/recording_fsm.hpp"
#include "recording/recording_context.hpp"
#include "recording/recording_engine.hpp"

using namespace screenrec;
using namespace screenrec::fsm;

namespace {

// Records every engine call and returns configurable results, so transitions
// can be tested without libobs.
class FakeEngine : public IRecordingEngine {
public:
    std::vector<std::string> calls;
    bool initializeResult = true;
    bool startResult = true;
    bool pauseResult = true;
    bool resumeResult = true;
    bool stopResult = true;

    bool initializeOBSCore() override {
        calls.emplace_back("initializeOBSCore");
        return true;
    }
    bool initialize(const RecordingConfig&) override {
        calls.emplace_back("initialize");
        return initializeResult;
    }
    bool start() override {
        calls.emplace_back("start");
        return startResult;
    }
    bool pause() override {
        calls.emplace_back("pause");
        return pauseResult;
    }
    bool resume() override {
        calls.emplace_back("resume");
        return resumeResult;
    }
    bool stop() override {
        calls.emplace_back("stop");
        return stopResult;
    }
    void shutdown() override { calls.emplace_back("shutdown"); }

    void setConfig(const RecordingConfig& config) override { config_ = config; }
    RecordingConfig getConfig() const override { return config_; }
    void setOutputPath(const std::string& path) override { outputPath_ = path; }
    std::string getOutputPath() const override { return outputPath_; }
    void setErrorCallback(ErrorCallback) override {}
    void setLogger(std::shared_ptr<LogService>) override {}

    uint64_t getFrameCount() const override { return 0; }
    uint64_t getDroppedFrames() const override { return 0; }
    uint64_t getTotalBytes() const override { return 0; }
    bool isRunning() const override { return false; }
    bool isPaused() const override { return false; }

    bool pauseAudio() override { return true; }
    bool pauseVideo() override { return true; }
    bool resumeAudio() override { return true; }
    bool resumeVideo() override { return true; }

    int countCalls(const std::string& name) const {
        int n = 0;
        for (const auto& c : calls) {
            if (c == name) { n++; }
        }
        return n;
    }

private:
    RecordingConfig config_;
    std::string outputPath_;
};

struct Harness {
    std::shared_ptr<FakeEngine> engine = std::make_shared<FakeEngine>();
    RecordingContext ctx{engine, nullptr, nullptr};
    RecordingFSM fsm{ctx};

    StartRecordingEvent startEvent() {
        StartRecordingEvent ev;
        ev.sessionId = "session_ab12cd34";
        ev.outputPath = "/tmp/session_ab12cd34.mp4";
        return ev;
    }

    void driveToRecording() {
        fsm.processEvent(startEvent());
        REQUIRE(ctx.getState() == RecordingState::RECORDING);
    }
};

}

TEST_CASE("Idle + Start with a working engine transitions to Recording") {
    Harness h;
    h.fsm.processEvent(h.startEvent());

    CHECK(h.ctx.getState() == RecordingState::RECORDING);
    CHECK(h.fsm.isRecording());
    CHECK(h.engine->countCalls("initialize") == 1);
    CHECK(h.engine->countCalls("start") == 1);
    CHECK(h.engine->getOutputPath() == "/tmp/session_ab12cd34.mp4");
    CHECK(h.ctx.getSessionId() == "session_ab12cd34");
    CHECK(h.ctx.getFilePath() == "/tmp/session_ab12cd34.mp4");
}

TEST_CASE("Idle + Start with a null engine lands in Error, not a crash") {
    auto ctx = RecordingContext(nullptr, nullptr, nullptr);
    RecordingFSM fsm(ctx);

    StartRecordingEvent ev;
    ev.sessionId = "session_00";
    ev.outputPath = "/tmp/x.mp4";
    fsm.processEvent(ev);

    CHECK(ctx.getState() == RecordingState::ERR);
    CHECK(ctx.getError() == "Recording engine is null");
}

TEST_CASE("Idle + Start with failing engine initialize lands in Error") {
    Harness h;
    h.engine->initializeResult = false;
    h.fsm.processEvent(h.startEvent());

    CHECK(h.ctx.getState() == RecordingState::ERR);
    CHECK(h.ctx.getError() == "Failed to start recording engine");
    // start() must not run after a failed initialize.
    CHECK(h.engine->countCalls("start") == 0);
}

TEST_CASE("Idle + Start with failing engine start lands in Error") {
    Harness h;
    h.engine->startResult = false;
    h.fsm.processEvent(h.startEvent());
    CHECK(h.ctx.getState() == RecordingState::ERR);
}

TEST_CASE("Recording + Pause/Resume round-trips through Paused") {
    Harness h;
    h.driveToRecording();

    h.fsm.processEvent(PauseRecordingEvent{});
    CHECK(h.ctx.getState() == RecordingState::PAUSED);
    CHECK(h.engine->countCalls("pause") == 1);

    h.fsm.processEvent(ResumeRecordingEvent{});
    CHECK(h.ctx.getState() == RecordingState::RECORDING);
    CHECK(h.engine->countCalls("resume") == 1);
}

TEST_CASE("failed engine pause leaves the context in Recording") {
    // Pins current behavior: the SML machine still moves to Paused, but the
    // externally visible context state stays RECORDING when pause() fails.
    Harness h;
    h.driveToRecording();
    h.engine->pauseResult = false;

    h.fsm.processEvent(PauseRecordingEvent{});
    CHECK(h.ctx.getState() == RecordingState::RECORDING);
}

TEST_CASE("Recording stops via Stop and via AutoStop") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(StopRecordingEvent{});
    CHECK(h.ctx.getState() == RecordingState::STOPPING);
    CHECK(h.engine->countCalls("stop") == 1);

    Harness h2;
    h2.driveToRecording();
    h2.fsm.processEvent(AutoStopEvent{});
    CHECK(h2.ctx.getState() == RecordingState::STOPPING);
}

TEST_CASE("Paused recording can be stopped directly") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(PauseRecordingEvent{});
    REQUIRE(h.ctx.getState() == RecordingState::PAUSED);

    h.fsm.processEvent(StopRecordingEvent{});
    CHECK(h.ctx.getState() == RecordingState::STOPPING);
}

TEST_CASE("capture failure while recording lands in Error with the message") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(CaptureFailureEvent{"display disconnected"});
    CHECK(h.ctx.getState() == RecordingState::ERR);
    CHECK(h.ctx.getError() == "display disconnected");
}

TEST_CASE("capture failure while paused lands in Error, not silently dropped") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(PauseRecordingEvent{});
    REQUIRE(h.ctx.getState() == RecordingState::PAUSED);
    h.fsm.processEvent(CaptureFailureEvent{"display disconnected while paused"});
    CHECK(h.ctx.getState() == RecordingState::ERR);
    CHECK(h.ctx.getError() == "display disconnected while paused");
}

TEST_CASE("finalize completes into Stopped with file metadata") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(StopRecordingEvent{});
    REQUIRE(h.ctx.getState() == RecordingState::STOPPING);

    h.fsm.processEvent(FinalizeCompleteEvent{"/tmp/final.mp4", 12345});
    CHECK(h.ctx.getState() == RecordingState::STOPPED);
    CHECK(h.ctx.getFilePath() == "/tmp/final.mp4");
    CHECK(h.ctx.getFileSizeBytes() == 12345);
}

TEST_CASE("finalize failure lands in Error") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(StopRecordingEvent{});
    h.fsm.processEvent(FinalizeFailureEvent{"mux failed"});
    CHECK(h.ctx.getState() == RecordingState::ERR);
    CHECK(h.ctx.getError() == "mux failed");
}

TEST_CASE("Stopped session is destroyed back to Idle; Error cleans up to Idle") {
    Harness h;
    h.driveToRecording();
    h.fsm.processEvent(StopRecordingEvent{});
    h.fsm.processEvent(FinalizeCompleteEvent{"/tmp/f.mp4", 1});
    h.fsm.processEvent(DestroySessionEvent{});
    CHECK(h.ctx.getState() == RecordingState::IDLE);

    Harness h2;
    h2.driveToRecording();
    h2.fsm.processEvent(CaptureFailureEvent{"boom"});
    h2.fsm.processEvent(CleanupEvent{});
    CHECK(h2.ctx.getState() == RecordingState::IDLE);
}

TEST_CASE("events with no transition from the current state are ignored") {
    Harness h;

    // Idle ignores everything except Start.
    h.fsm.processEvent(PauseRecordingEvent{});
    h.fsm.processEvent(ResumeRecordingEvent{});
    h.fsm.processEvent(StopRecordingEvent{});
    h.fsm.processEvent(FinalizeCompleteEvent{"/tmp/x.mp4", 1});
    CHECK(h.ctx.getState() == RecordingState::IDLE);
    CHECK(h.engine->calls.empty());

    // Recording ignores a second Start: the engine must not re-initialize.
    h.driveToRecording();
    h.fsm.processEvent(h.startEvent());
    CHECK(h.engine->countCalls("initialize") == 1);
    CHECK(h.ctx.getState() == RecordingState::RECORDING);

    // Stopped ignores Start (a finished session cannot be restarted).
    h.fsm.processEvent(StopRecordingEvent{});
    h.fsm.processEvent(FinalizeCompleteEvent{"/tmp/y.mp4", 2});
    REQUIRE(h.ctx.getState() == RecordingState::STOPPED);
    h.fsm.processEvent(h.startEvent());
    CHECK(h.engine->countCalls("initialize") == 1);
    CHECK(h.ctx.getState() == RecordingState::STOPPED);
}
