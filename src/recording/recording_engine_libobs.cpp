#include "recording/recording_engine_libobs.hpp"
#include "recording/capture_sources.hpp"
#include "recording/encoder_selection.hpp"
#include "recording/monitor_layout.hpp"
#include "recording/output_dimensions.hpp"
#include "recording/stop_reason.hpp"
#include "logging/log_service.hpp"
#include "obs/obs-data.h"
#include "obs/obs.h"
#include <array>
#include <cstddef>
#include <mutex>
#include <obs/obs-module.h>
#include <obs/util/platform.h>
#include <obs/util/base.h>
#include <chrono>
#include <string>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ShellScalingApi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#pragma comment(lib, "Shcore.lib")
#elif defined(__APPLE__)
#include "platform/macos_display.hpp"
#include "platform/macos_permissions.hpp"
#include "platform/process_path.hpp"
#include <filesystem>
#endif

namespace screenrec {
namespace {
#ifdef __APPLE__
// OBS runtime root; uses executableDir(), not cwd — launchd starts agents with cwd "/". OBS_PATH overrides.
std::string obsRuntimeRootMac() {
    if (const char* obsPath = std::getenv("OBS_PATH")) { // NOLINT(concurrency-mt-unsafe)
        return obsPath;
    }
    namespace fs = std::filesystem;
    std::string exeDir = platform::executableDir();
    if (!fs::exists(exeDir + "/PlugIns") &&
        fs::exists(fs::path(exeDir).parent_path() / "PlugIns")) {
        return fs::path(exeDir).parent_path().string();
    }
    return exeDir;
}

// Renderer dylib name varies by packaging; os_dlopen appends ".so" to bare names on macOS, so resolve the absolute path up front.
std::string findGraphicsModuleMac(const std::string& obsRoot) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(obsRoot + "/Frameworks", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("libobs-opengl", 0) == 0 &&
            name.size() >= 6 && name.compare(name.size() - 6, 6, ".dylib") == 0) {
            return entry.path().string();
        }
    }
    return "libobs-opengl";
}
#endif

using encoder::bitrateKbps;
using encoder::isHevcEncoder;
using encoder::keyintSec;
using encoder::maxBitrateKbps;
using encoder::RateControl;
using encoder::resolveRateControl;
using encoder::wantsHevc;

#ifdef _WIN32
void killMuxProcesses() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            if (_wcsicmp(entry.szExeFile, L"obs-ffmpeg-mux.exe") != 0) {
                continue;
            }
            if (const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                    entry.th32ProcessID)) {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &entry) != 0);
    }
    CloseHandle(snapshot);
}
#endif

void applyX264RateControl(obs_data_t* settings, const VideoConfig& video) {
    switch (resolveRateControl(video)) {
    case RateControl::CBR:
        obs_data_set_string(settings, "rate_control", "CBR");
        obs_data_set_int(settings, "bitrate", bitrateKbps(video));
        break;
    case RateControl::VBR:
        obs_data_set_string(settings, "rate_control", "VBR");
        obs_data_set_int(settings, "bitrate", bitrateKbps(video));
        obs_data_set_int(settings, "buffer_size", maxBitrateKbps(video));
        obs_data_set_bool(settings, "use_bufsize", true);
        break;
    case RateControl::QUALITY:
        obs_data_set_string(settings, "rate_control", "CRF");
        obs_data_set_int(settings, "crf", video.crf);
        break;
    }
}
}

// No caching: a cache filled before obs modules load would stay empty forever, silently forcing the x264 fallback.
std::unordered_set<std::string> getAvailableEncoderTypes() {
    std::unordered_set<std::string> out;
    size_t idx = 0;
    const char* encoderId = nullptr;
    while (obs_enum_encoder_types(idx++, &encoderId)) {
        if (encoderId != nullptr) {
            out.insert(encoderId);
        }
    }
    return out;
}
RecordingEngine::RecordingEngine() : logger_(LogService::noop()) {}

void RecordingEngine::setLogger(std::shared_ptr<LogService> logger) {
    if (logger) {
        logger_ = std::move(logger);
    } else {
        logger_ = LogService::noop();
    }
}

RecordingEngine::~RecordingEngine() {
    shutdown();
}

bool RecordingEngine::initializeOBSCore() {
    std::scoped_lock lock(mutex_);

    if (obsCoreInitialized_) {
        logger_->debug("RecordingEngine", "OBS core already initialized (singleton)");
        return true;
    }

    logger_->info("RecordingEngine", "Initializing OBS core (one-time initialization)...");

    if (!initializeOBS()) {
        logger_->error("RecordingEngine", "Failed to initialize OBS core");
        return false;
    }

    obsCoreInitialized_ = true;
    logger_->info("RecordingEngine", "OBS core initialized successfully");
    return true;
}

bool RecordingEngine::initialize(const RecordingConfig& config) {
    std::scoped_lock lock(mutex_);

    config_ = config;

    logger_->info("RecordingEngine", "Initializing recording session...");
    logger_->debug("RecordingEngine", "Video: " + std::to_string(config.video.width) + "x" +
                                          std::to_string(config.video.height) + " @ " + std::to_string(config.video.fps) + " fps");

#ifdef __APPLE__
    // Without the TCC grant SCK delivers silently black frames; fail the start and let the request prompt.
    if (!macos::screenCaptureAccessPreflight()) {
        macos::screenCaptureAccessRequest();
        reportError("Screen recording permission not granted for screen_recording_agent. "
                    "Grant it in System Settings > Privacy & Security > Screen Recording, then retry.");
        return false;
    }
#endif

    // Core is already up (initializeOBSCore), but per-session video/audio settings must be reset here.
    if (!initializeOBS()) {
        logger_->error("RecordingEngine", "Failed to initialize/reconfigure OBS");
        return false;
    }
    obsCoreInitialized_ = true;

    if (!createSources()) {
        logger_->error("RecordingEngine", "Failed to create sources");
        releaseOBS();
        return false;
    }

    if (!createEncoder()) {
        logger_->error("RecordingEngine", "Failed to create encoder");
        releaseOBS();
        return false;
    }

    if (!createOutput()) {
        logger_->error("RecordingEngine", "Failed to create output");
        releaseOBS();
        return false;
    }

    obsInitialized_ = true;
    logger_->info("RecordingEngine", "Recording session initialized successfully");
    return true;
}

bool RecordingEngine::initializeOBS() {
    logger_->debug("RecordingEngine", "Initializing OBS core...");

#ifdef _WIN32
    logger_->debug("RecordingEngine", "Initializing DPI awareness...");
    if (!initializeDPIAwareness()) {
        reportWarning("Failed to initialize DPI awareness");
    } else {
        logger_->debug("RecordingEngine", "DPI awareness initialized");
    }

    // Paths must be set before obs_startup, use forward slashes, and end with a trailing slash; root = OBS_PATH or cwd.
    {
        std::string obsRoot;
        if (const char* obsPath = std::getenv("OBS_PATH"); obsPath != nullptr) {
            obsRoot = obsPath;
            logger_->debug("RecordingEngine", "Using OBS_PATH env var: " + obsRoot);
        } else {
            std::array<char, MAX_PATH> currentPath{};
            GetCurrentDirectoryA(MAX_PATH, currentPath.data());
            obsRoot = currentPath.data();
            logger_->debug("RecordingEngine", "Current directory: " + obsRoot);
        }
        std::replace(obsRoot.begin(), obsRoot.end(), '\\', '/');

        const std::string libobsDataPath = obsRoot + "/data/libobs/";
        const std::string modulePath = obsRoot + "/obs-plugins/64bit";
        const std::string moduleDataPath = obsRoot + "/data/obs-plugins/%module%";

        logger_->debug("RecordingEngine", "Setting libobs data path: " + libobsDataPath);
        logger_->debug("RecordingEngine", "Setting OBS module path: " + modulePath);

        obs_add_data_path(libobsDataPath.c_str());
        obs_add_module_path(modulePath.c_str(), moduleDataPath.c_str());
    }
#elif defined(__APPLE__)
    // Explicit paths into the runtime next to the executable — never rely on NSBundle heuristics from a LaunchAgent.
    {
        const std::string obsRoot = obsRuntimeRootMac();

        const std::string modulePath = obsRoot + "/PlugIns/%module%.plugin/Contents/MacOS";
        const std::string moduleDataPath = obsRoot + "/PlugIns/%module%.plugin/Contents/Resources";
        const std::string libobsDataPath = obsRoot + "/Frameworks/libobs.framework/Resources/";

        logger_->debug("RecordingEngine", "OBS root: " + obsRoot);
        logger_->debug("RecordingEngine", "Setting OBS module path: " + modulePath);

        obs_add_data_path(libobsDataPath.c_str());
        obs_add_module_path(modulePath.c_str(), moduleDataPath.c_str());
    }
#endif

    logger_->info("RecordingEngine", "Starting OBS...");
    if (!obs_initialized()) {
        base_set_log_handler([](int logLevel, const char* format, va_list args, void* param) {
            auto* engine = static_cast<RecordingEngine*>(param);
            if (engine == nullptr || !engine->logger_) {
                return;
            }
            // OBS logs per-frame at debug; skip the formatting cost when debug output is filtered anyway.
            const bool isWarning = logLevel <= LOG_WARNING;
            if (!isWarning && engine->logger_->getLevel() > LogLevel::DEBUG) {
                return;
            }
            std::array<char, 4096> buffer{};
            vsnprintf(buffer.data(), buffer.size(), format, args);
            if (isWarning) {
                engine->logger_->warning("OBS", std::string(buffer.data()));
            } else {
                engine->logger_->debug("OBS", std::string(buffer.data()));
            }
        },
            this);

        if (!obs_startup("en-US", nullptr, nullptr)) {
            reportError("Failed to initialize OBS - obs_startup returned false");
            return false;
        }
        logger_->info("RecordingEngine", "OBS startup successful");
    } else {
        logger_->debug("RecordingEngine", "OBS already initialized");
    }

    logger_->debug("RecordingEngine", "Detecting monitors...");
    detectMonitors();
    const std::size_t detectedMonitors = monitors_.size();
    monitors_ = selectRecordedMonitors(monitors_);
    if (monitors_.size() < detectedMonitors) {
        logger_->warning("RecordingEngine",
            "Detected " + std::to_string(detectedMonitors) + " monitors; recording caps at " +
                std::to_string(MAX_RECORDED_MONITORS) + " (main + 2 extended), excluding " +
                std::to_string(detectedMonitors - monitors_.size()) + " monitor(s)");
    }
    if (config_.video.layout != "native") {
        normalizeEqualTiles(monitors_);
        logger_->info("RecordingEngine", "Equal-tile layout: monitors scaled to a common tile height");
    }
    if (!monitors_.empty() && monitors_[0].width > 0 && monitors_[0].height > 0) {
        logger_->info("RecordingEngine", "Detected " + std::to_string(monitors_.size()) + " monitor(s)");
        logger_->debug("RecordingEngine", "Primary monitor: " + std::to_string(monitors_[0].width) + "x" +
                                              std::to_string(monitors_[0].height) + " (DPI scale: " +
                                              std::to_string(monitors_[0].dpiScaleX) + "x" + std::to_string(monitors_[0].dpiScaleY) + ")");
    } else {
        logger_->warning("RecordingEngine", "No monitors detected or invalid monitor info");
    }

    logger_->info("RecordingEngine", "Initializing video: " + std::to_string(config_.video.width) + "x" +
                                         std::to_string(config_.video.height) + " @ " + std::to_string(config_.video.fps) + " fps");
#ifdef _WIN32
    logger_->debug("RecordingEngine", "Using graphics module: libobs-d3d11");
#else
    logger_->debug("RecordingEngine", "Using graphics module: libobs-opengl");
#endif

    struct obs_video_info ovi = {};
    ovi.fps_num = config_.video.fps;
    ovi.fps_den = 1;
#ifdef _WIN32
    // D3D11 captures faster than OpenGL on Windows; OpenGL is the fallback below.
    ovi.graphics_module = "libobs-d3d11";
    logger_->debug("RecordingEngine", "Attempting Direct3D 11 graphics module...");
#else
    const std::string graphicsModulePath = findGraphicsModuleMac(obsRuntimeRootMac());
    ovi.graphics_module = graphicsModulePath.c_str();
    logger_->debug("RecordingEngine", "Using graphics module: " + graphicsModulePath);
#endif
    MonitorBounds bounds;
    if (getCompositeBounds(monitors_, bounds)) {
        const int compositeWidth = bounds.maxX - bounds.minX;
        const int compositeHeight = bounds.maxY - bounds.minY;
        ovi.base_width = compositeWidth > 0 ? static_cast<uint32_t>(compositeWidth) : monitors_[0].width;
        ovi.base_height = compositeHeight > 0 ? static_cast<uint32_t>(compositeHeight) : monitors_[0].height;
    } else {
        // No monitors (Session 0, headless, RDP disconnect): use configured dimensions instead of indexing an empty vector.
        logger_->warning("RecordingEngine",
            "No monitors detected; falling back to configured dimensions for base canvas");
        ovi.base_width = config_.video.width;
        ovi.base_height = config_.video.height;
    }
    // Height-cap scaling (see output_dimensions.hpp); the configured width does not constrain the frame.
    const OutputDimensions out = computeOutputDimensions(
        ovi.base_width, ovi.base_height, config_.video.height);
    ovi.output_width = out.width;
    ovi.output_height = out.height;
    logger_->info("RecordingEngine",
        "Output resolution: " + std::to_string(out.width) + "x" + std::to_string(out.height) +
            " (canvas " + std::to_string(ovi.base_width) + "x" + std::to_string(ovi.base_height) + ")");
    if (config_.video.width != 0 && config_.video.width != out.width) {
        logger_->debug("RecordingEngine",
            "Configured video.width=" + std::to_string(config_.video.width) +
                " is informational only; output width " + std::to_string(out.width) +
                " follows the canvas aspect under the height cap");
    }
    ovi.output_format = VIDEO_FORMAT_NV12;
    ovi.adapter = 0;
    ovi.gpu_conversion = true;
    ovi.colorspace = VIDEO_CS_709;
    ovi.range = VIDEO_RANGE_PARTIAL;
    ovi.scale_type = OBS_SCALE_AREA;

    const bool videoParamsSame = videoContextReady_ && obs_get_video() != nullptr &&
                                 lastBaseWidth_ == ovi.base_width && lastBaseHeight_ == ovi.base_height &&
                                 lastOutputWidth_ == ovi.output_width && lastOutputHeight_ == ovi.output_height &&
                                 lastFpsNum_ == ovi.fps_num;
    if (videoParamsSame) {
        logger_->info("RecordingEngine", "Reusing existing video context; skipping obs_reset_video");
    } else {
        int ret = obs_reset_video(&ovi);

#ifdef _WIN32
        if (ret != OBS_VIDEO_SUCCESS) {
            logger_->warning("RecordingEngine", "D3D11 failed (error " + std::to_string(ret) + "), trying OpenGL fallback...");
            ovi.graphics_module = "libobs-opengl";
            ret = obs_reset_video(&ovi);
        }
#endif

        if (ret != OBS_VIDEO_SUCCESS) {
            std::string errorMsg = "Failed to reset video. Error code: " + std::to_string(ret);
            switch (ret) {
            case -1: errorMsg += " (MODULE_NOT_FOUND)"; break;
            case -2: errorMsg += " (MODULE_ERROR)"; break;
            case -3: errorMsg += " (INVALID_PARAM)"; break;
            case -4: errorMsg += " (CURRENTLY_ACTIVE)"; break;
            case -5: errorMsg += " (MODULE_ALREADY_LOADED)"; break;
            default: errorMsg += " (UNKNOWN)"; break;
            }
            reportError(errorMsg);
            return false;
        }

        lastBaseWidth_ = ovi.base_width;
        lastBaseHeight_ = ovi.base_height;
        lastOutputWidth_ = ovi.output_width;
        lastOutputHeight_ = ovi.output_height;
        lastFpsNum_ = ovi.fps_num;
        videoContextReady_ = true;
        ++videoResetGeneration_;
        logger_->info("RecordingEngine", std::string("Video initialized successfully using ") + ovi.graphics_module);
    }

    logger_->info("RecordingEngine", "Initializing audio subsystem: " +
                                         std::to_string(config_.audio.sampleRate) + " Hz, " +
                                         std::to_string(config_.audio.channels) + " channel(s)");

    struct obs_audio_info oai = {};
    oai.samples_per_sec = config_.audio.sampleRate;
    oai.speakers = config_.audio.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;

    const bool audioParamsSame = audioContextReady_ && obs_get_audio() != nullptr &&
                                 lastSampleRate_ == oai.samples_per_sec &&
                                 lastSpeakers_ == static_cast<int>(oai.speakers);
    if (audioParamsSame) {
        logger_->info("RecordingEngine", "Reusing existing audio context; skipping obs_reset_audio");
    } else if (!obs_reset_audio(&oai)) {
        reportError("Failed to reset audio");
        return false;
    } else {
        lastSampleRate_ = oai.samples_per_sec;
        lastSpeakers_ = static_cast<int>(oai.speakers);
        audioContextReady_ = true;
        ++audioResetGeneration_;
    }

    if (config_.audio.enabled) {
        logger_->info("RecordingEngine", "Audio capture enabled");
    } else {
        logger_->info("RecordingEngine", "Audio capture disabled (but subsystem initialized)");
    }

    static bool modulesLoaded = false;
    if (!modulesLoaded) {
        // Windows needs an explicit module list; elsewhere OBS auto-loads.
#ifdef _WIN32
        logger_->info("RecordingEngine", "Loading required OBS modules...");
        if (!loadRequiredModules()) {
            reportError("Failed to load required modules");
            return false;
        }
        logger_->debug("RecordingEngine", "Required modules loaded successfully");
#else
        logger_->info("RecordingEngine", "Loading OBS modules...");
        obs_load_all_modules();
        obs_log_loaded_modules();
#endif

        obs_post_load_modules();
        modulesLoaded = true;
        logger_->info("RecordingEngine", "OBS modules initialized");
    } else {
        logger_->debug("RecordingEngine", "OBS modules already loaded, skipping");
    }

    return true;
}

bool RecordingEngine::createSources() {
    logger_->debug("RecordingEngine", "Creating sources...");

    {
        logger_->debug("RecordingEngine", "=== Available source types ===");
        const char* sourceId = nullptr;
        size_t idx = 0;
        while (obs_enum_source_types(idx++, &sourceId)) {
            if (sourceId != nullptr) {
                logger_->debug("RecordingEngine", std::string("  Source type: ") + sourceId);
            }
        }
        logger_->debug("RecordingEngine", "==============================");
    }

    scene_ = obs_scene_create("MainScene");
    if (scene_ == nullptr) {
        reportError("Failed to create scene");
        return false;
    }
    logger_->debug("RecordingEngine", "Scene created");

    const auto reportErrorFn = [this](const std::string& msg) { reportError(msg); };
    const auto reportWarningFn = [this](const std::string& msg) { reportWarning(msg); };

    if (!capture::createScreenSources(scene_, monitors_, sceneItems_, screenCaptures_,
            &displaySource_, reportErrorFn, logger_)) {
        return false;
    }

    if (config_.audio.enabled) {
        logger_->debug("RecordingEngine", "Creating audio sources...");
        capture::createAudioSources(&audioSource_, &micSource_, reportWarningFn, logger_);
    }

    if (config_.audio.enabled && config_.audio.noiseSuppression != "none") {
        capture::applyNoiseSuppression(micSource_, config_.audio.noiseSuppression, "Microphone", logger_);
        capture::applyNoiseSuppression(audioSource_, config_.audio.noiseSuppression, "DesktopAudio", logger_);
    }

    return true;
}

bool RecordingEngine::retileDisplays(float* fitScaleOut) {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load() || scene_ == nullptr) {
        return false;
    }

    detectMonitors();
    monitors_ = selectRecordedMonitors(monitors_);
    if (monitors_.empty()) {
        // Transient zero-display state (dock unplugged): keep the last scene; a later change retiles.
        logger_->warning("RecordingEngine", "Retile skipped: no displays detected");
        return false;
    }
    if (config_.video.layout != "native") {
        normalizeEqualTiles(monitors_);
    }

    struct obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi)) {
        reportWarning("Retile skipped: video pipeline info unavailable");
        return false;
    }

    MonitorBounds bounds;
    if (!getCompositeBounds(monitors_, bounds)) {
        return false;
    }
    const CanvasFit fit = fitCompositeToCanvas(ovi.base_width, ovi.base_height,
        bounds.maxX - bounds.minX,
        bounds.maxY - bounds.minY);
    if (fitScaleOut != nullptr) {
        *fitScaleOut = fit.scale;
    }

    // Video-pause hides scene items; rebuilt items must stay hidden or a retile would silently un-pause video.
    const bool videoHidden = !sceneItems_.empty() && sceneItems_[0] != nullptr &&
                             !obs_sceneitem_visible(sceneItems_[0]);

    for (auto* item : sceneItems_) {
        if (item != nullptr) {
            obs_sceneitem_remove(item);
        }
    }
    sceneItems_.clear();
    for (auto* source : screenCaptures_) {
        if (source != nullptr) {
            obs_source_release(source);
        }
    }
    screenCaptures_.clear();
    if (displaySource_ != nullptr) {
        obs_source_release(displaySource_);
        displaySource_ = nullptr;
    }

    // WARNING, not ERROR: a failed live retile must not surface as a recording failure; the next display change retries.
    const auto reportWarningFn = [this](const std::string& msg) { reportWarning(msg); };
    if (!capture::createScreenSources(scene_, monitors_, sceneItems_, screenCaptures_,
            &displaySource_, reportWarningFn, logger_)) {
        reportWarning("Retile: no capture sources created for new display set");
        return false;
    }

    // The factory places items for a virgin canvas; apply the fit so the composite fills the fixed canvas.
    for (auto* item : sceneItems_) {
        if (item == nullptr) {
            continue;
        }
        struct vec2 pos = {};
        obs_sceneitem_get_pos(item, &pos);
        vec2_set(&pos, (pos.x * fit.scale) + fit.offsetX, (pos.y * fit.scale) + fit.offsetY);
        obs_sceneitem_set_pos(item, &pos);

        struct vec2 tile = {};
        obs_sceneitem_get_bounds(item, &tile);
        vec2_set(&tile, tile.x * fit.scale, tile.y * fit.scale);
        obs_sceneitem_set_bounds(item, &tile);
        obs_sceneitem_set_scale_filter(item, OBS_SCALE_AREA);

        if (videoHidden) {
            obs_sceneitem_set_visible(item, false);
        }
    }
    if (videoHidden) {
        sceneItemVisibility_.assign(sceneItems_.size(), true); // resume shows all
    }

    logger_->info("RecordingEngine",
        "Retiled to " + std::to_string(monitors_.size()) + " display(s), scale " +
            std::to_string(fit.scale) + " into fixed canvas " +
            std::to_string(ovi.base_width) + "x" + std::to_string(ovi.base_height));
    return true;
}

bool RecordingEngine::captureSourcesHealthy() {
    std::scoped_lock lock(mutex_);
    if (!isRunning_.load() || screenCaptures_.empty()) {
        return true;
    }
    if (!sceneItems_.empty() && sceneItems_[0] != nullptr &&
        !obs_sceneitem_visible(sceneItems_[0])) {
        return true;
    }
    return std::all_of(screenCaptures_.begin(), screenCaptures_.end(), [](obs_source_t* source) {
        return source != nullptr && obs_source_get_width(source) != 0 &&
               obs_source_get_height(source) != 0;
    });
}

std::string RecordingEngine::detectBestVideoEncoder() {
    logger_->debug("RecordingEngine", "Detecting best available video encoder...");

    const auto available = getAvailableEncoderTypes();

    {
        logger_->debug("RecordingEngine", "Scanning for available H264/H265 video encoders:");
        int encoderCount = 0;
        for (const auto& encoderId : available) {
            const char* codec = obs_get_encoder_codec(encoderId.c_str());
            if (codec != nullptr && (strcmp(codec, "h264") == 0 || strcmp(codec, "h265") == 0)) {
                logger_->info("RecordingEngine", std::string("  ✓ Found: ") + encoderId + " (codec: " + codec + ")");
                encoderCount++;
            }
        }
        if (encoderCount == 0) {
            logger_->info("RecordingEngine", "  (No H264/H265 encoders detected yet - may load during module init)");
        }
    }

    const auto choice = encoder::selectVideoEncoder(config_.video, available);
    for (const auto& warning : choice.warnings) {
        logger_->warning("RecordingEngine", warning);
    }
    for (const auto& note : choice.info) {
        logger_->info("RecordingEngine", note);
    }
    return choice.id;
}

bool RecordingEngine::createEncoder() {
    std::string encoderId = detectBestVideoEncoder();
    bool isHardwareEncoder = (encoderId != "obs_x264");
    videoEncoderId_ = encoderId;
    videoEncoderIsHardware_ = isHardwareEncoder;

    obs_data_t* videoSettings = obs_data_create();

    const RateControl rc = resolveRateControl(config_.video);

    if (isHardwareEncoder) {
        obs_data_set_int(videoSettings, "keyint_sec", keyintSec(config_.video));

        if (encoderId.find("nvenc") != std::string::npos) {
            if (rc == RateControl::CBR) {
                obs_data_set_string(videoSettings, "rate_control", "CBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
            } else if (rc == RateControl::VBR) {
                obs_data_set_string(videoSettings, "rate_control", "VBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
                obs_data_set_int(videoSettings, "max_bitrate", maxBitrateKbps(config_.video));
            } else {
                obs_data_set_string(videoSettings, "rate_control", "CQP");
                obs_data_set_int(videoSettings, "cqp", config_.video.crf);
            }
            obs_data_set_string(videoSettings, "preset", "p5"); // p1=fastest .. p7=slowest
            obs_data_set_string(videoSettings, "profile", isHevcEncoder(encoderId) ? "main" : "high");
            obs_data_set_string(videoSettings, "tune", "hq");
            // no B-frames/look-ahead: queued frames stall the stop flush at low fps
            obs_data_set_int(videoSettings, "bf", 0);
            obs_data_set_bool(videoSettings, "lookahead", false);
        } else if (encoderId.find("amf") != std::string::npos) {
            // AMF has no max_bitrate key; VBR peak is auto (~1.5x bitrate).
            if (rc == RateControl::CBR) {
                obs_data_set_string(videoSettings, "rate_control", "CBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
            } else if (rc == RateControl::VBR) {
                obs_data_set_string(videoSettings, "rate_control", "VBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
            } else {
                obs_data_set_string(videoSettings, "rate_control", "CQP");
                obs_data_set_int(videoSettings, "cqp_i", 24);
                obs_data_set_int(videoSettings, "cqp_p", config_.video.crf);
            }
            obs_data_set_string(videoSettings, "preset", "quality");
            obs_data_set_string(videoSettings, "profile", isHevcEncoder(encoderId) ? "main" : "high");
            obs_data_set_int(videoSettings, "bf", 0);
        } else if (encoderId.find("qsv") != std::string::npos) {
            if (rc == RateControl::CBR) {
                obs_data_set_string(videoSettings, "rate_control", "CBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
            } else if (rc == RateControl::VBR) {
                obs_data_set_string(videoSettings, "rate_control", "VBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
                obs_data_set_int(videoSettings, "max_bitrate", maxBitrateKbps(config_.video));
            } else {
                obs_data_set_string(videoSettings, "rate_control", "ICQ");
                obs_data_set_int(videoSettings, "icq_quality", config_.video.crf);
            }
            obs_data_set_string(videoSettings, "target_usage", "quality");
            obs_data_set_string(videoSettings, "profile", isHevcEncoder(encoderId) ? "main" : "high");
            obs_data_set_int(videoSettings, "bframes", 0);
            obs_data_set_int(videoSettings, "async_depth", 1); // in-flight queue drains at 1/fps per slot on stop
        } else if (encoderId.find("com.apple.videotoolbox") != std::string::npos) {
            // VideoToolbox: true CBR needs Apple Silicon + macOS 13+ (guaranteed by deployment target); VBR maps to ABR with a ceiling.
            if (rc == RateControl::CBR) {
                obs_data_set_string(videoSettings, "rate_control", "CBR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
            } else if (rc == RateControl::VBR) {
                obs_data_set_string(videoSettings, "rate_control", "ABR");
                obs_data_set_int(videoSettings, "bitrate", bitrateKbps(config_.video));
                obs_data_set_bool(videoSettings, "limit_bitrate", true);
                obs_data_set_int(videoSettings, "max_bitrate", maxBitrateKbps(config_.video));
                obs_data_set_double(videoSettings, "max_bitrate_window", 1.5);
            } else {
                // VT quality is 0-100 (higher = better), inverted from x264-style crf; crf 28 -> 44.
                const int quality = std::max(1, std::min(100, 100 - (static_cast<int>(config_.video.crf) * 2)));
                obs_data_set_string(videoSettings, "rate_control", "CRF");
                obs_data_set_int(videoSettings, "quality", quality);
            }
            obs_data_set_string(videoSettings, "profile", isHevcEncoder(encoderId) ? "main" : "high");
            obs_data_set_bool(videoSettings, "bframes", true);
        }

        logger_->info("RecordingEngine", "Creating GPU-accelerated video encoder: " + encoderId);
    } else {
        applyX264RateControl(videoSettings, config_.video);
        obs_data_set_string(videoSettings, "preset", config_.video.preset.c_str());
        obs_data_set_string(videoSettings, "profile", "high");
        obs_data_set_string(videoSettings, "tune", "film"); // Better for screen content than zerolatency
        obs_data_set_int(videoSettings, "keyint_sec", keyintSec(config_.video));
        obs_data_set_int(videoSettings, "bf", 2); // B-frames (obs-x264 key is "bf", not "bframes")

        logger_->info("RecordingEngine", "Creating software video encoder: x264 (optimized for screen content)");
    }

    {
        std::string rcMsg;
        switch (rc) {
        case RateControl::CBR:
            rcMsg = "Rate control: CBR @ " + std::to_string(bitrateKbps(config_.video)) + " kbps (hard size cap)";
            break;
        case RateControl::VBR:
            rcMsg = "Rate control: VBR @ " + std::to_string(bitrateKbps(config_.video)) + " kbps target / " + std::to_string(maxBitrateKbps(config_.video)) + " kbps max (capped, quality-first under ceiling)";
            break;
        case RateControl::QUALITY:
            rcMsg = "Rate control: constant quality (crf/cqp " + std::to_string(config_.video.crf) + ", UNBOUNDED size)";
            break;
        }
        logger_->info("RecordingEngine", rcMsg);
    }

    const char* settingsJson = obs_data_get_json(videoSettings);
    const std::string settingsKey = settingsJson != nullptr ? settingsJson : "";
    const bool reusable = videoEncoder_ != nullptr && !encoderPoisoned_ &&
                          cachedEncoderId_ == encoderId &&
                          encoderVideoGeneration_ == videoResetGeneration_;

    if (reusable) {
        if (cachedEncoderSettings_ != settingsKey) {
            obs_encoder_update(videoEncoder_, videoSettings);
            cachedEncoderSettings_ = settingsKey;
        }
        obs_data_release(videoSettings);
        obs_encoder_set_video(videoEncoder_, obs_get_video());
        logger_->info("RecordingEngine", "Reusing video encoder across sessions: " + encoderId);
    } else {
        if (videoEncoder_ != nullptr) {
            obs_encoder_release(videoEncoder_);
            videoEncoder_ = nullptr;
        }
        videoEncoder_ = obs_video_encoder_create(encoderId.c_str(), "video_encoder", videoSettings, nullptr);
        obs_data_release(videoSettings);

        if (videoEncoder_ == nullptr) {
            reportError("Failed to create video encoder: " + encoderId);

            if (isHardwareEncoder) {
                logger_->warning("RecordingEngine", "Hardware encoder failed, trying software fallback...");

                obs_data_t* fallbackSettings = obs_data_create();
                applyX264RateControl(fallbackSettings, config_.video);
                obs_data_set_string(fallbackSettings, "preset", "veryfast");
                obs_data_set_string(fallbackSettings, "profile", "high");
                obs_data_set_string(fallbackSettings, "tune", "zerolatency");
                obs_data_set_int(fallbackSettings, "keyint_sec", keyintSec(config_.video));

                videoEncoder_ = obs_video_encoder_create("obs_x264", "video_encoder", fallbackSettings, nullptr);
                obs_data_release(fallbackSettings);

                if (videoEncoder_ == nullptr) {
                    reportError("Failed to create fallback x264 encoder");
                    cachedEncoderId_.clear();
                    return false;
                }

                logger_->info("RecordingEngine", "Successfully created x264 fallback encoder");
                videoEncoderId_ = "obs_x264";
                videoEncoderIsHardware_ = false;
            } else {
                cachedEncoderId_.clear();
                return false;
            }
        }

        obs_encoder_set_video(videoEncoder_, obs_get_video());
        cachedEncoderId_ = videoEncoderId_;
        cachedEncoderSettings_ = videoEncoderId_ == encoderId ? settingsKey : std::string();
        encoderVideoGeneration_ = videoResetGeneration_;
        encoderPoisoned_ = false;

        logger_->info("RecordingEngine", "Video encoder created successfully: " +
                                             std::string(obs_encoder_get_name(videoEncoder_)));
    }

    // Created even with audio disabled: some outputs require an audio encoder.
    obs_data_t* audioSettings = obs_data_create();
    obs_data_set_int(audioSettings, "bitrate", config_.audio.enabled ? config_.audio.bitrate / 1000 : 128);
    const char* audioJson = obs_data_get_json(audioSettings);
    const std::string audioKey = audioJson != nullptr ? audioJson : "";

    if (audioEncoder_ != nullptr && encoderAudioGeneration_ == audioResetGeneration_) {
        if (cachedAudioSettings_ != audioKey) {
            obs_encoder_update(audioEncoder_, audioSettings);
            cachedAudioSettings_ = audioKey;
        }
        obs_data_release(audioSettings);
        obs_encoder_set_audio(audioEncoder_, obs_get_audio());
    } else {
        if (audioEncoder_ != nullptr) {
            obs_encoder_release(audioEncoder_);
            audioEncoder_ = nullptr;
        }
        audioEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "aac_encoder", audioSettings, 0, nullptr);
        obs_data_release(audioSettings);

        if (audioEncoder_ == nullptr) {
            logger_->warning("RecordingEngine", "Failed to create audio encoder");
        } else {
            logger_->info("RecordingEngine", "Audio encoder created");
            obs_encoder_set_audio(audioEncoder_, obs_get_audio());
            cachedAudioSettings_ = audioKey;
            encoderAudioGeneration_ = audioResetGeneration_;
        }
    }

    return true;
}

bool RecordingEngine::createOutput() {
    logger_->debug("RecordingEngine", "Creating output...");

    if (outputPath_.empty()) {
        reportError("Output path is empty");
        return false;
    }

    logger_->info("RecordingEngine", "Output file path: " + outputPath_);
    logger_->debug("RecordingEngine", "Output format: " + config_.outputFormat);

    {
        logger_->debug("RecordingEngine", "Enumerating available output types:");
        size_t idx = 0;
        const char* outputId = nullptr;
        while (obs_enum_output_types(idx++, &outputId)) {
            logger_->debug("RecordingEngine", std::string("  Output type: ") + outputId);
        }
    }

    obs_data_t* outputSettings = obs_data_create();
    obs_data_set_string(outputSettings, "path", outputPath_.c_str());
    // No format_name: the muxer infers the container from the file extension.

    // Fragmented MP4: fragments finalize as written, so a crash loses at most the tail after the last keyframe.
    if (config_.outputFormat == "mp4") {
        obs_data_set_string(outputSettings, "muxer_settings",
            "movflags=frag_keyframe+empty_moov+default_base_moof frag_duration=5000000");
    }

    output_ = obs_output_create("ffmpeg_muxer", "output", outputSettings, nullptr);
    obs_data_release(outputSettings);

    if (output_ == nullptr) {
        logger_->error("RecordingEngine", "Failed to create output - ffmpeg_muxer not found or invalid");
        reportError("Failed to create output");
        return false;
    }

    logger_->debug("RecordingEngine", "FFmpeg muxer output created");

    obs_output_set_video_encoder(output_, videoEncoder_);
    logger_->debug("RecordingEngine", "Video encoder connected to output");

    if (audioEncoder_ != nullptr) {
        obs_output_set_audio_encoder(output_, audioEncoder_, 0);
        logger_->debug("RecordingEngine", "Audio encoder connected to output");
    }

    signal_handler_t* handler = obs_output_get_signal_handler(output_);
    signal_handler_connect(handler, "start", onStartSignal, this);
    signal_handler_connect(handler, "stop", onStopSignal, this);
    signal_handler_connect(handler, "stopping", onStoppingSignal, this);

    logger_->debug("RecordingEngine", "Output created successfully");
    return true;
}

bool RecordingEngine::start() {
    std::scoped_lock lock(mutex_);

    if (isRunning_.load()) {
        logger_->warning("RecordingEngine", "Cannot start: already running");
        return false;
    }

    if (!obsInitialized_.load()) {
        logger_->error("RecordingEngine", "Cannot start: not initialized (call initialize() first)");
        return false;
    }

    logger_->info("RecordingEngine", "Starting recording output...");

    // the scene must be assigned to output channel 0 or the output captures no video
    if (scene_ != nullptr) {
        obs_source_t* sceneSource = obs_scene_get_source(scene_);
        obs_set_output_source(0, sceneSource);
        logger_->debug("RecordingEngine", "Scene set as video output source (channel 0)");
    } else {
        logger_->error("RecordingEngine", "Scene is null, cannot set output source");
        return false;
    }

    // Audio sources capture nothing until assigned to an output channel.
    if (audioSource_ != nullptr) {
        obs_set_output_source(1, audioSource_);
        logger_->debug("RecordingEngine", "Desktop audio set as output source (channel 1)");
    }
    if (micSource_ != nullptr) {
        obs_set_output_source(2, micSource_);
        logger_->debug("RecordingEngine", "Microphone set as output source (channel 2)");
    }

    outputStopped_.store(false);
    lastStopCode_.store(stop_reason::CODE_SUCCESS);

    if (!obs_output_start(output_)) {
        const char* error = obs_output_get_last_error(output_);
        logger_->error("RecordingEngine", std::string("obs_output_start failed: ") + ((error != nullptr) ? error : "unknown"));

        // Hardware start failures are common with QSV on large/odd resolutions; retry once on software x264.
        if (videoEncoderIsHardware_) {
            logger_->warning("RecordingEngine", "Hardware encoder failed to start output. Retrying with software x264...");

            if (videoEncoder_ != nullptr) {
                obs_encoder_release(videoEncoder_);
                videoEncoder_ = nullptr;
            }

            obs_data_t* fallbackSettings = obs_data_create();
            applyX264RateControl(fallbackSettings, config_.video);
            obs_data_set_string(fallbackSettings, "preset", "veryfast");
            obs_data_set_string(fallbackSettings, "profile", "high");
            obs_data_set_string(fallbackSettings, "tune", "zerolatency");
            obs_data_set_int(fallbackSettings, "keyint_sec", keyintSec(config_.video));

            videoEncoder_ = obs_video_encoder_create("obs_x264", "video_encoder", fallbackSettings, nullptr);
            obs_data_release(fallbackSettings);

            if (videoEncoder_ != nullptr) {
                obs_encoder_set_video(videoEncoder_, obs_get_video());
                obs_output_set_video_encoder(output_, videoEncoder_);
                videoEncoderId_ = "obs_x264";
                videoEncoderIsHardware_ = false;
                cachedEncoderId_ = "obs_x264";
                cachedEncoderSettings_.clear();
                encoderVideoGeneration_ = videoResetGeneration_;
                encoderPoisoned_ = false;

                if (obs_output_start(output_)) {
                    isRunning_.store(true);
                    isPaused_.store(false);
                    logger_->info("RecordingEngine", "Recording output started successfully with x264 fallback");
                    return true;
                }
                const char* retryError = obs_output_get_last_error(output_);
                logger_->error("RecordingEngine", std::string("obs_output_start failed (x264 fallback): ") + ((retryError != nullptr) ? retryError : "unknown"));
                reportError(std::string("Failed to start output (x264 fallback): ") + ((retryError != nullptr) ? retryError : "unknown"));
                return false;
            }

            reportError("Failed to create x264 fallback encoder");
            return false;
        }

        reportError(std::string("Failed to start output: ") + ((error != nullptr) ? error : "unknown"));
        return false;
    }

    isRunning_.store(true);
    isPaused_.store(false);

    logger_->info("RecordingEngine", "Recording output started successfully");
    return true;
}

bool RecordingEngine::pause() {
    if (!isRunning_.load() || isPaused_.load()) {
        return false;
    }

    // libobs has no native pause: mute audio and hide video instead, keeping the output alive.
    if (!pauseAudio() || !pauseVideo()) {
        logger_->warning("RecordingEngine", "Failed to pause audio/video sources");
        return false;
    }

    isPaused_.store(true);

    return true;
}

bool RecordingEngine::resume() {
    if (!isRunning_.load() || !isPaused_.load()) {
        return false;
    }

    if (!resumeAudio() || !resumeVideo()) {
        logger_->warning("RecordingEngine", "Failed to resume audio/video sources");
        return false;
    }

    isPaused_.store(false);

    return true;
}

bool RecordingEngine::stop() {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load()) {
        return false;
    }

    logger_->info("RecordingEngine", "Stopping recording...");

    obs_output_stop(output_);

    {
        // encoder queue drains at capture rate; cap below the 30s IPC stop timeout
        const int fps = config_.video.fps > 0 ? config_.video.fps : 30;
        const int waitSec = std::min(25, 5 + ((10 + fps - 1) / fps));
        std::unique_lock<std::mutex> stopLock(outputStoppedMutex_);
        bool stopped = outputStoppedCV_.wait_for(stopLock, std::chrono::seconds(waitSec), [this] {
            return outputStopped_.load();
        });
        if (!stopped) {
            logger_->warning("RecordingEngine",
                "Output did not stop gracefully within " + std::to_string(waitSec) + "s, forcing stop");
            encoderPoisoned_ = true;
            obs_output_force_stop(output_);
            if (!outputStoppedCV_.wait_for(stopLock, std::chrono::seconds(5), [this] {
                    return outputStopped_.load();
                })) {
                logger_->warning("RecordingEngine",
                    "Output still not confirmed stopped after force stop");
#ifdef _WIN32
                killMuxProcesses();
                if (!outputStoppedCV_.wait_for(stopLock, std::chrono::seconds(3), [this] {
                        return outputStopped_.load();
                    })) {
                    logger_->error("RecordingEngine",
                        "Output unresponsive even after killing the mux process");
                }
#endif
            }
        }
    }

    isRunning_.store(false);
    isPaused_.store(false);

    if (lastStopCode_.load() != stop_reason::CODE_SUCCESS) {
        encoderPoisoned_ = true;
    }

    if (output_ != nullptr) {
        const int totalFrames = obs_output_get_total_frames(output_);
        const int droppedFrames = obs_output_get_frames_dropped(output_);
        logger_->info("RecordingEngine",
            "Encode telemetry: encoder=" + (videoEncoderId_.empty() ? "unknown" : videoEncoderId_) +
                " totalFrames=" + std::to_string(totalFrames) +
                " droppedFrames=" + std::to_string(droppedFrames) +
                " bytes=" + std::to_string(totalBytes_.load()));
    }

    if (obsInitialized_.load()) {
        logger_->debug("RecordingEngine", "Releasing recording session resources");
        releaseOBS();
        obsInitialized_.store(false);
    }

    logger_->info("RecordingEngine", "Recording stopped and session resources released");
    return true;
}

void RecordingEngine::shutdown() {
    stop();

    std::scoped_lock lock(mutex_);

    if (videoEncoder_ != nullptr) {
        obs_encoder_release(videoEncoder_);
        videoEncoder_ = nullptr;
    }
    if (audioEncoder_ != nullptr) {
        obs_encoder_release(audioEncoder_);
        audioEncoder_ = nullptr;
    }

    if (obsCoreInitialized_.load()) {
        logger_->info("RecordingEngine", "Shutting down OBS core");
        obs_shutdown();
        obsCoreInitialized_.store(false);
        logger_->info("RecordingEngine", "OBS core shutdown complete");
    }
}

void RecordingEngine::trimGraphicsMemory() {
#ifdef _WIN32
    std::scoped_lock lock(mutex_);
    if (!obsCoreInitialized_.load() || !obs_initialized()) {
        return;
    }
    obs_enter_graphics();
    auto* device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (device != nullptr) {
        IDXGIDevice3* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice3),
                reinterpret_cast<void**>(&dxgiDevice)))) {
            dxgiDevice->Trim();
            dxgiDevice->Release();
            logger_->info("RecordingEngine", "Graphics driver memory trim requested");
        }
    }
    obs_leave_graphics();
#endif
}

void RecordingEngine::releaseOBS() {
    // Clear output channels BEFORE releasing sources: OBS holds its own refs; skipping leaks them ("duplicate name" next session).
    obs_set_output_source(0, nullptr);
    obs_set_output_source(1, nullptr);
    obs_set_output_source(2, nullptr);

    if (output_ != nullptr) {
        signal_handler_t* handler = obs_output_get_signal_handler(output_);
        if (handler != nullptr) {
            signal_handler_disconnect(handler, "start", onStartSignal, this);
            signal_handler_disconnect(handler, "stop", onStopSignal, this);
            signal_handler_disconnect(handler, "stopping", onStoppingSignal, this);
        }
        obs_output_release(output_);
        output_ = nullptr;
    }

    // Encoders deliberately survive the session: recreating them leaks a QSV/oneVPL worker pool (~10 threads,
    // ~40 MB) per destroy; shutdown() releases them, createEncoder() replaces them on change.

    for (auto* source : screenCaptures_) {
        if (source != nullptr) {
            obs_source_release(source);
        }
    }
    screenCaptures_.clear();
    sceneItems_.clear();
    sceneItemVisibility_.clear();

    if (displaySource_ != nullptr) {
        obs_source_release(displaySource_);
        displaySource_ = nullptr;
    }

    if (audioSource_ != nullptr) {
        obs_source_release(audioSource_);
        audioSource_ = nullptr;
    }

    if (micSource_ != nullptr) {
        obs_source_release(micSource_);
        micSource_ = nullptr;
    }

    if (scene_ != nullptr) {
        obs_scene_release(scene_);
        scene_ = nullptr;
    }
}

void RecordingEngine::onStartSignal(void* data, calldata_t* cd) {
    auto* engine = static_cast<RecordingEngine*>(data);
    if (engine != nullptr) engine->handleOutputSignal("start");
}

void RecordingEngine::onStopSignal(void* data, calldata_t* cd) {
    auto* engine = static_cast<RecordingEngine*>(data);
    long long code = stop_reason::CODE_SUCCESS;
    if (cd != nullptr) {
        calldata_get_int(cd, "code", &code);
    }
    if (engine != nullptr) engine->handleOutputSignal("stop", static_cast<int>(code));
}

void RecordingEngine::onStoppingSignal(void* data, calldata_t* cd) {
    auto* engine = static_cast<RecordingEngine*>(data);
    if (engine != nullptr) engine->handleOutputSignal("stopping");
}

void RecordingEngine::handleOutputSignal(const char* signal, int stopCode) {
    if (strcmp(signal, "stop") != 0) {
        return;
    }
    if (output_ != nullptr) {
        totalBytes_ = obs_output_get_total_bytes(output_);
        frameCount_ = obs_output_get_total_frames(output_);
        droppedFrames_ = obs_output_get_frames_dropped(output_);
    }
    lastStopCode_.store(stopCode);
    if (stopCode != stop_reason::CODE_SUCCESS) {
        // Disk-full/encoder/write failures arrive as a spontaneous stop with a non-zero code; the callback surfaces them to the agent.
        reportError(stop_reason::classify(stopCode).message);
    }
    {
        std::scoped_lock stopLock(outputStoppedMutex_);
        outputStopped_.store(true);
    }
    outputStoppedCV_.notify_all();
}

uint64_t RecordingEngine::liveEncodedFrames() {
    std::scoped_lock lock(mutex_);
    if (!isRunning_.load() || output_ == nullptr) {
        return 0;
    }
    return static_cast<uint64_t>(obs_output_get_total_frames(output_));
}

void RecordingEngine::reportError(const std::string& error) {
    logger_->error("RecordingEngine", error);

    if (errorCallback_) {
        errorCallback_(error);
    }
}

// Non-fatal: recording continues, so errorCallback_ (a recording failure to CTI) is deliberately NOT fired.
void RecordingEngine::reportWarning(const std::string& message) {
    logger_->warning("RecordingEngine", message);
}

bool RecordingEngine::pauseAudio() {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load()) {
        return false;
    }

    if (micSource_ != nullptr) {
        storedMicVolume_ = obs_source_get_volume(micSource_);
        obs_source_set_volume(micSource_, 0.0f);
    }

    if (audioSource_ != nullptr) {
        storedDesktopAudioVolume_ = obs_source_get_volume(audioSource_);
        obs_source_set_volume(audioSource_, 0.0f);
    }

    return true;
}

bool RecordingEngine::resumeAudio() {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load()) {
        return false;
    }

    if (micSource_ != nullptr) {
        obs_source_set_volume(micSource_, storedMicVolume_);
    }

    if (audioSource_ != nullptr) {
        obs_source_set_volume(audioSource_, storedDesktopAudioVolume_);
    }

    return true;
}

bool RecordingEngine::pauseVideo() {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load()) {
        return false;
    }

    sceneItemVisibility_.clear();

    for (auto& sceneItem : sceneItems_) {
        if (sceneItem != nullptr) {
            sceneItemVisibility_.push_back(obs_sceneitem_visible(sceneItem));
            obs_sceneitem_set_visible(sceneItem, false);
        }
    }

    return true;
}

bool RecordingEngine::resumeVideo() {
    std::scoped_lock lock(mutex_);

    if (!isRunning_.load()) {
        return false;
    }

    for (size_t i = 0; i < sceneItems_.size() && i < sceneItemVisibility_.size(); i++) {
        if (sceneItems_[i] != nullptr) {
            obs_sceneitem_set_visible(sceneItems_[i], sceneItemVisibility_[i]);
        }
    }

    sceneItemVisibility_.clear();
    return true;
}

std::vector<MonitorInfo> RecordingEngine::detectMonitors() {
#ifdef _WIN32
    monitors_.clear();

    struct EnumData {
        std::vector<MonitorInfo>* monitors;
    };

    auto monitorEnumProc = [](HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) -> BOOL {
        EnumData* data = reinterpret_cast<EnumData*>(dwData);

        MONITORINFOEX mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hMonitor, &mi)) {
            MonitorInfo info;
            info.index = static_cast<int>(data->monitors->size());
            info.name = std::string(mi.szDevice);

            info.x = mi.rcMonitor.left;
            info.y = mi.rcMonitor.top;
            info.width = mi.rcMonitor.right - mi.rcMonitor.left;
            info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
            info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            UINT dpiX = 96;
            UINT dpiY = 96;
            if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                info.dpiScaleX = dpiX / 96.0f;
                info.dpiScaleY = dpiY / 96.0f;
            }

            data->monitors->push_back(info);
        }

        return TRUE;
    };

    EnumData data{&monitors_};
    EnumDisplayMonitors(nullptr, nullptr, monitorEnumProc, reinterpret_cast<LPARAM>(&data));

    return monitors_;
#else
    monitors_ = macos::detectMonitors();
    if (monitors_.empty()) {
        logger_->warning("RecordingEngine", "CGGetActiveDisplayList returned no displays");
    }
    return monitors_;
#endif
}

#ifdef _WIN32
bool RecordingEngine::initializeDPIAwareness() {
    // Windows 10 1703+ API
    typedef BOOL(WINAPI * SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pfnSetProcessDpiAwarenessContext =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pfnSetProcessDpiAwarenessContext) {
            if (pfnSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
                return true;
            }
        }
    }

    // Windows 8.1+ API
    typedef HRESULT(WINAPI * SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        auto pfnSetProcessDpiAwareness =
            (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (pfnSetProcessDpiAwareness) {
            if (SUCCEEDED(pfnSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
                FreeLibrary(shcore);
                return true;
            }
        }
        FreeLibrary(shcore);
    }

    return SetProcessDPIAware() != 0;
}

bool RecordingEngine::loadModule(const std::string& binPath, const std::string& dataPath,
    const std::string& moduleName) {
    obs_module_t* module = nullptr;
    std::string modulePath = binPath + "/" + moduleName + ".dll";

    int code = obs_open_module(&module, modulePath.c_str(), dataPath.c_str());
    if (code != MODULE_SUCCESS) {
        return false;
    }

    if (!obs_init_module(module)) {
        return false;
    }

    return true;
}

bool RecordingEngine::loadRequiredModules() {
    // Modules are copied next to the executable at build time; cwd is the executable dir at startup.
    std::array<char, MAX_PATH> currentPath{};
    GetCurrentDirectoryA(MAX_PATH, currentPath.data());
    std::string obsRoot = std::string(currentPath.data());
    std::replace(obsRoot.begin(), obsRoot.end(), '\\', '/');

    std::string binPath = obsRoot + "/obs-plugins/64bit";
    std::string dataPath = obsRoot + "/data/obs-plugins";

    logger_->info("RecordingEngine", "Loading OBS modules from: " + binPath);

    std::vector<std::string> requiredModules = {
        "win-capture",
        "win-wasapi",
        "obs-outputs",
        "obs-ffmpeg",
        "obs-x264",
        "obs-filters" // noise suppression (noise_suppress_filter)
    };

    std::vector<std::string> optionalGpuModules = {
        "obs-nvenc",
        "obs-qsv11"};

    bool allRequiredLoaded = true;

    for (const auto& module : requiredModules) {
        std::string moduleData = dataPath + "/" + module;
        if (!loadModule(binPath, moduleData, module)) {
            logger_->error("RecordingEngine", "Failed to load REQUIRED module: " + module);
            allRequiredLoaded = false;
        } else {
            logger_->info("RecordingEngine", "Successfully loaded module: " + module);
        }
    }

    for (const auto& module : optionalGpuModules) {
        std::string moduleData = dataPath + "/" + module;
        if (!loadModule(binPath, moduleData, module)) {
            logger_->info("RecordingEngine", "GPU encoder not available: " + module + " (this is OK)");
        } else {
            logger_->info("RecordingEngine", "Successfully loaded GPU encoder: " + module);
        }
    }

    return allRequiredLoaded;
}
#endif

}
