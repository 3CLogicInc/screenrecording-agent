#pragma once

#include <obs/obs.h>
#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <utility>
#include "../types.hpp"
#include "recording_engine.hpp"

namespace screenrec {

class LogService;

class RecordingEngine : public IRecordingEngine {
public:
    RecordingEngine();
    ~RecordingEngine() override;

    // Call once per agent.
    bool initializeOBSCore() override;

    bool initialize(const RecordingConfig& config) override;
    bool start() override;
    bool pause() override;
    bool resume() override;
    bool stop() override;
    void shutdown() override;

    void setConfig(const RecordingConfig& config) override { config_ = config; }
    RecordingConfig getConfig() const override { return config_; }

    void setOutputPath(const std::string& path) override { outputPath_ = path; }
    std::string getOutputPath() const override { return outputPath_; }

    void setErrorCallback(ErrorCallback callback) override { errorCallback_ = std::move(callback); }

    void setLogger(std::shared_ptr<LogService> logger) override; // null -> LogService::noop()

    uint64_t getFrameCount() const override { return frameCount_.load(); }
    uint64_t getDroppedFrames() const override { return droppedFrames_.load(); }
    uint64_t getTotalBytes() const override { return totalBytes_.load(); }

    bool isRunning() const override { return isRunning_.load(); }
    bool isPaused() const override { return isPaused_.load(); }

    // OBS_OUTPUT_* code from the last "stop" signal; non-zero = died or finalized dirty (see stop_reason.hpp).
    int lastStopCode() const { return lastStopCode_.load(); }

    // Rebuilds capture sources into the running fixed canvas; scene mutation only, safe while output is active.
    bool retileDisplays(float* fitScaleOut = nullptr);

    bool captureSourcesHealthy();

    uint64_t liveEncodedFrames();

    bool pauseAudio() override;
    bool pauseVideo() override;
    bool resumeAudio() override;
    bool resumeVideo() override;

    std::vector<MonitorInfo> detectMonitors();

    void trimGraphicsMemory();

private:
    bool initializeOBS();

    bool createSources();
    bool createEncoder();
    bool createOutput();

    std::string detectBestVideoEncoder();

    void releaseOBS();

    static void onStartSignal(void* data, calldata_t* cd);
    static void onStopSignal(void* data, calldata_t* cd);
    static void onStoppingSignal(void* data, calldata_t* cd);

    void handleOutputSignal(const char* signal, int stopCode = 0);
    void reportError(const std::string& error);     // logs ERROR + fires errorCallback_
    void reportWarning(const std::string& message); // logs WARNING only (non-fatal)

#ifdef _WIN32
    static bool initializeDPIAwareness();
    static bool loadModule(const std::string& binPath, const std::string& dataPath, const std::string& moduleName);
    bool loadRequiredModules();
#endif

    RecordingConfig config_;
    std::string outputPath_;

    obs_source_t* displaySource_ = nullptr;
    obs_source_t* audioSource_ = nullptr;
    obs_source_t* micSource_ = nullptr;
    obs_encoder_t* videoEncoder_ = nullptr;
    obs_encoder_t* audioEncoder_ = nullptr;
    obs_output_t* output_ = nullptr;
    obs_scene_t* scene_ = nullptr;
    std::string videoEncoderId_;
    bool videoEncoderIsHardware_ = false;

    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isPaused_{false};
    std::atomic<bool> obsCoreInitialized_{false};
    std::atomic<bool> obsInitialized_{false};

    bool videoContextReady_ = false;
    bool audioContextReady_ = false;
    uint32_t lastBaseWidth_ = 0;
    uint32_t lastBaseHeight_ = 0;
    uint32_t lastOutputWidth_ = 0;
    uint32_t lastOutputHeight_ = 0;
    uint32_t lastFpsNum_ = 0;
    uint32_t lastSampleRate_ = 0;
    int lastSpeakers_ = -1;

    std::string cachedEncoderId_;
    std::string cachedEncoderSettings_;
    std::string cachedAudioSettings_;
    uint32_t videoResetGeneration_ = 0;
    uint32_t audioResetGeneration_ = 0;
    uint32_t encoderVideoGeneration_ = 0;
    uint32_t encoderAudioGeneration_ = 0;
    bool encoderPoisoned_ = false;

    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> totalBytes_{0};
    std::atomic<int> lastStopCode_{0};

    ErrorCallback errorCallback_;

    mutable std::mutex mutex_;

    std::shared_ptr<LogService> logger_;

    float storedMicVolume_ = 1.0f;
    float storedDesktopAudioVolume_ = 1.0f;
    std::vector<bool> sceneItemVisibility_;
    std::vector<obs_sceneitem_t*> sceneItems_;
    std::vector<obs_source_t*> screenCaptures_;

    std::vector<MonitorInfo> monitors_;

    std::condition_variable outputStoppedCV_;
    std::mutex outputStoppedMutex_;
    std::atomic<bool> outputStopped_{false};
};

}
