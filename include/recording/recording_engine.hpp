#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "types.hpp"

namespace screenrec {

class LogService;

class IRecordingEngine {
public:
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~IRecordingEngine() = default;

    // OBS core is per-process; the rest is per recording session.
    virtual bool initializeOBSCore() = 0;
    virtual bool initialize(const RecordingConfig& config) = 0;
    virtual bool start() = 0;
    virtual bool pause() = 0;
    virtual bool resume() = 0;
    virtual bool stop() = 0;
    virtual void shutdown() = 0;

    virtual void setConfig(const RecordingConfig& config) = 0;
    [[nodiscard]] virtual RecordingConfig getConfig() const = 0;
    virtual void setOutputPath(const std::string& path) = 0;
    [[nodiscard]] virtual std::string getOutputPath() const = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;
    virtual void setLogger(std::shared_ptr<LogService> logger) = 0;

    [[nodiscard]] virtual uint64_t getFrameCount() const = 0;
    [[nodiscard]] virtual uint64_t getDroppedFrames() const = 0;
    [[nodiscard]] virtual uint64_t getTotalBytes() const = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;
    [[nodiscard]] virtual bool isPaused() const = 0;

    // Independent audio/video pause (CTI "Pause (Screen + Voice)" controls).
    virtual bool pauseAudio() = 0;
    virtual bool pauseVideo() = 0;
    virtual bool resumeAudio() = 0;
    virtual bool resumeVideo() = 0;
};

}
