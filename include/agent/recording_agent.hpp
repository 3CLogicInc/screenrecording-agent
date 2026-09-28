#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "constants.hpp"
#include "core/configuration_manager.hpp"
#include "ipc/ipc_message.hpp"
#include "ipc/ipc_transport.hpp"
#include "ipc/protocol.hpp"
#include "logging/log_service.hpp"
#include "recording/recording_context.hpp"
#include "recording/recording_engine_libobs.hpp"
#include "recording/recording_session.hpp"
#include "recording/stop_reason.hpp"
#include "storage/file_manager.hpp"
#include "storage/session_journal.hpp"
#include "types.hpp"
#ifdef __APPLE__
#include "platform/macos_display.hpp"
#include "platform/macos_permissions.hpp"
#include "platform/macos_status_indicator.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace screenrec::agent {

using json = nlohmann::json;
using ipc::IPCMessage;
using ipc::MessageType;

#ifdef __APPLE__
// Exit after this long without permission: only a freshly launchd-started process observes a TCC grant.
constexpr int RELAUNCH_WHEN_DENIED_SECS = 20;
#endif

constexpr int AGENT_MONITOR_INTERVAL_SEC = 5;
constexpr int ZERO_FRAME_FAIL_TICKS = 8;
constexpr int STATUS_LOG_EVERY_POLLS = 20;
// Stop cleanly below this floor instead of letting the muxer die mid-fragment on OBS_OUTPUT_NO_SPACE.
constexpr uint64_t MIN_FREE_DISK_BYTES = 500ULL * 1024 * 1024;
// Wall-vs-monotonic divergence above this between ticks means the machine slept; shorter naps record through.
constexpr int64_t SLEEP_GAP_THRESHOLD_NS = 30LL * 1000000000LL;
// Bounds encoder-runtime leaks: restart only between recordings, well above a fresh agent's ~15-25 threads.
constexpr int IDLE_RESTART_THREAD_THRESHOLD = 80;
constexpr uint64_t IDLE_RESTART_PRIVATE_BYTES = 400ULL * 1024 * 1024;

class RecordingAgent {
public:
    RecordingAgent() : running_(false) {
        LoggingConfig logConfig;
        logConfig.level = LogLevel::INFO;
        logConfig.processTag = "agent";
        logConfig.enableFileLogging = true;
        logConfig.logDirectory = constants::logsDir();
        logConfig.enableLokiReporting = false; // Don't send to Loki from agent, service handles it

        // Only the rotation knobs come from config.json; level and Loki stay agent-pinned above.
        ConfigurationManager fileConfig;
        if (fileConfig.loadFromFile(ConfigurationManager::defaultConfigPath())) {
            const auto fileLogging = fileConfig.getLoggingConfig();
            logConfig.maxFileSizeBytes = fileLogging.maxFileSizeBytes;
            logConfig.maxFiles = fileLogging.maxFiles;
        }

        logger_ = std::make_shared<LogService>(logConfig);
        logger_->initialize();
        logger_->info("Agent", "Recording Agent initializing...");

        engine_ = std::make_shared<RecordingEngine>();
        engine_->setLogger(logger_);

        // Fired from OBS threads: must not take sessionMutex_ (the stop path holds it); monitorLoop drains it.
        engine_->setErrorCallback([this](const std::string& error) {
            {
                std::scoped_lock lock(failureMutex_);
                pendingFailureMessage_ = error;
            }
            pendingEngineFailure_.store(true);
        });

        if (!engine_->initializeOBSCore()) {
            logger_->error("Agent", "FATAL: Failed to initialize OBS core");
            throw std::runtime_error("Failed to initialize OBS core");
        }
        logger_->info("Agent", "OBS core initialized successfully (singleton)");

        StorageConfig storageConfig;
        storageConfig.outputDirectory = constants::recordingsDir();
        fileManager_ = std::make_shared<FileManager>(storageConfig);

#ifdef __APPLE__
        launchedWithScreenPermission_ = macos::screenCaptureAccessPreflight();
        if (!launchedWithScreenPermission_) {
            logger_->warning("Agent",
                "Screen recording permission not granted yet. Grant in "
                "System Settings > Privacy & Security > Screen Recording");
            // At most one prompt across relaunches (marker file): re-prompting stacks dialogs and can wedge one.
            if (!macos::tccPromptAlreadyShown()) {
                macos::markTccPromptShown();
                macos::screenCaptureAccessRequest();
            }
        }
#endif

        logger_->info("Agent", "Agent initialized - ready for recording sessions");
    }

    ~RecordingAgent() {
        stop();
    }

    RecordingAgent(const RecordingAgent&) = delete;
    RecordingAgent& operator=(const RecordingAgent&) = delete;
    RecordingAgent(RecordingAgent&&) = delete;
    RecordingAgent& operator=(RecordingAgent&&) = delete;

    bool start() {
        logger_->info("Agent", "Connecting to service IPC...");

        pipeClient_ = ipc::createIpcClient(constants::IPC_ENDPOINT);

        // Must run before connect(): connect() starts the reader thread, so a later set is a data race.
        pipeClient_->setMessageHandler([this](const IPCMessage& msg) {
            handleMessage(msg);
        });

        if (!pipeClient_->connect(constants::PIPE_CONNECT_TIMEOUT_MS)) {
            logger_->error("Agent", "Failed to connect to service");
            return false;
        }

        logger_->info("Agent", "Connected to service");

        running_ = true;

        IPCMessage readyMsg;
        readyMsg.type = MessageType::RESPONSE;
        readyMsg.action = ipc::protocol::ACTION_READY;
        {
            ipc::protocol::ReadyPayload ready;
            ready.protocolVersion = ipc::protocol::PROTOCOL_VERSION;
            ready.agentVersion = constants::APP_VERSION;
#ifdef __APPLE__
            ready.platform = "macos";
            ready.screenRecordingPermission = macos::screenCaptureAccessPreflight() ? 1 : 0;
            ready.monitorCount = static_cast<int>(macos::detectMonitors().size());
#else
            ready.platform = "windows";
            ready.monitorCount = GetSystemMetrics(SM_CMONITORS);
#endif
            readyMsg.payload = ready.toJson().dump();
        }
        readyMsg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(readyMsg);

        heartbeatThread_ = std::thread(&RecordingAgent::heartbeatLoop, this);
        monitorThread_ = std::thread(&RecordingAgent::monitorLoop, this);

#ifdef _WIN32
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS powerParams = {};
        powerParams.Callback = &RecordingAgent::onPowerEvent;
        powerParams.Context = this;
        powerNotify_ = RegisterSuspendResumeNotification(&powerParams, DEVICE_NOTIFY_CALLBACK);
        if (powerNotify_ == nullptr) {
            logger_->warning("Agent", "Suspend/resume notification registration failed");
        }
#endif

        logger_->info("Agent", "Agent ready and running");
        return true;
    }

    void stop() {
        // Not gated on running_ (a thread left joinable would std::terminate); every step below is idempotent.
        bool wasRunning = false;
        {
            std::scoped_lock lock(shutdownMutex_);
            wasRunning = running_.exchange(false);
        }
        if (wasRunning) {
            logger_->info("Agent", "Stopping agent...");
        }
        shutdownCV_.notify_all();

#ifdef _WIN32
        if (powerNotify_ != nullptr) {
            UnregisterSuspendResumeNotification(powerNotify_);
            powerNotify_ = nullptr;
        }
#endif

        // Finalize BEFORE dropping the pipe so the upload request still reaches the daemon.
        if (pipeClient_) {
            std::scoped_lock lock(sessionMutex_);
            finalizeAndUploadLocked(/*sendStopResponse=*/false, journal::END_SHUTDOWN);
        }

        // disconnect() joins the reader thread: no handler can touch currentSession_ after this point.
        if (pipeClient_) {
            pipeClient_->disconnect();
        }

        if (heartbeatThread_.joinable()) {
            heartbeatThread_.join();
        }
        if (monitorThread_.joinable()) {
            monitorThread_.join();
        }

        // A start that raced shutdown may have recreated the session; pipe is down, so the rescan uploads it.
        {
            std::scoped_lock lock(sessionMutex_);
            finalizeAndUploadLocked(/*sendStopResponse=*/false, journal::END_SHUTDOWN);
        }
#ifdef __APPLE__
        macos::hideRecordingIndicator();
#endif

        logger_->info("Agent", "Agent stopped");
    }

    void run(const std::atomic<bool>& stopRequested) {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (stopRequested.load()) {
                logger_->info("Agent", "Termination signal received; shutting down");
                running_ = false;
                break;
            }

            if (!pipeClient_->isConnected()) {
                logger_->error("Agent", "Lost connection to service");
                running_ = false;
                break;
            }

#ifdef __APPLE__
            // Exit so launchd relaunches us: only a fresh process observes a grant.
            if (!launchedWithScreenPermission_ && ++deniedSeconds_ >= RELAUNCH_WHEN_DENIED_SECS) {
                logger_->info("Agent",
                    "Screen recording permission still not granted; restarting to re-check "
                    "(grant it in System Settings > Privacy & Security > Screen Recording)");
                running_ = false;
                break;
            }
#endif
        }
    }

private:
    void handleMessage(const IPCMessage& message) {
        if (message.action == ipc::protocol::ACTION_GET_STATUS) {
            logger_->debug("Agent", "Received message: " + message.action);
        } else {
            logger_->info("Agent", "Received message: " + message.action);
        }

        activeRequestId_ = message.requestId;

        try {
            if (message.action == ipc::protocol::ACTION_START_RECORDING) {
                handleStartRecording(message);
            } else if (message.action == ipc::protocol::ACTION_STOP_RECORDING) {
                handleStopRecording(message);
            } else if (message.action == ipc::protocol::ACTION_CONFIGURE) {
                handleConfigure(message);
            } else if (message.action == ipc::protocol::ACTION_PAUSE_RECORDING) {
                handlePauseRecording(message);
            } else if (message.action == ipc::protocol::ACTION_RESUME_RECORDING) {
                handleResumeRecording(message);
            } else if (message.action == ipc::protocol::ACTION_PAUSE_AUDIO) {
                handlePauseAudio(message);
            } else if (message.action == ipc::protocol::ACTION_PAUSE_VIDEO) {
                handlePauseVideo(message);
            } else if (message.action == ipc::protocol::ACTION_RESUME_AUDIO) {
                handleResumeAudio(message);
            } else if (message.action == ipc::protocol::ACTION_RESUME_VIDEO) {
                handleResumeVideo(message);
            } else if (message.action == ipc::protocol::ACTION_APPEND_METADATA) {
                handleAppendMetadata(message);
            } else if (message.action == ipc::protocol::ACTION_GET_STATUS) {
                handleGetStatus();
            } else if (message.action == ipc::protocol::ACTION_SHUTDOWN) {
                handleShutdown(message);
            } else {
                logger_->warning("Agent", "Unknown command: " + message.action);
            }
        } catch (const std::exception& e) {
            logger_->error("Agent", std::string("Error handling message: ") + e.what());
            sendError(message.sessionId, e.what());
        }
    }

    void handleStartRecording(const IPCMessage& message) {
        std::scoped_lock lock(sessionMutex_);

        if (currentSession_) {
            // Same session -> idempotent no-op upstream; different session -> 409, never masked as success.
            const bool sameSession = message.sessionId == currentSessionId_;
            sendErrorCode(message.sessionId,
                sameSession ? ipc::protocol::ERR_ALREADY_RECORDING
                            : ipc::protocol::ERR_SESSION_CONFLICT,
                "Agent is already recording (session: " + currentSessionId_ + ")");
            return;
        }

        RecordingConfig config;
        std::string outputPath;

        try {
            json j = json::parse(message.payload);

            config.video.width = j.value("videoWidth", constants::DEFAULT_VIDEO_WIDTH);
            config.video.height = j.value("videoHeight", constants::DEFAULT_VIDEO_HEIGHT);
            config.video.fps = j.value("videoFps", constants::DEFAULT_VIDEO_FPS);
            config.video.bitrate = j.value("videoBitrate", constants::DEFAULT_VIDEO_BITRATE);
            config.video.maxBitrate = j.value("videoMaxBitrate", constants::DEFAULT_VIDEO_MAX_BITRATE);
            config.video.keyintSec = j.value("videoKeyintSec", constants::DEFAULT_VIDEO_KEYINT_SEC);
            config.video.rateControl = j.value("videoRateControl", constants::DEFAULT_VIDEO_RATE_CONTROL);
            config.video.crf = j.value("videoCrf", constants::DEFAULT_VIDEO_CRF);
            config.video.codec = j.value("videoCodec", constants::DEFAULT_VIDEO_CODEC);
            config.video.preset = j.value("videoPreset", constants::DEFAULT_VIDEO_PRESET);
            config.video.encoder = j.value("videoEncoder", "auto");
            config.video.layout = j.value("videoLayout", constants::DEFAULT_VIDEO_LAYOUT);

            config.audio.enabled = j.value("audioEnabled", constants::DEFAULT_AUDIO_ENABLED);
            config.audio.sampleRate = j.value("audioSampleRate", constants::DEFAULT_AUDIO_SAMPLE_RATE);
            config.audio.channels = j.value("audioChannels", constants::DEFAULT_AUDIO_CHANNELS);
            config.audio.bitrate = j.value("audioBitrate", constants::DEFAULT_AUDIO_BITRATE);
            config.audio.noiseSuppression = j.value("audioNoiseSuppression", AudioConfig{}.noiseSuppression);

            config.outputFormat = j.value("outputFormat", constants::DEFAULT_OUTPUT_FORMAT);
            config.maxDurationSeconds = j.value("maxDuration", constants::DEFAULT_MAX_DURATION_SECONDS);

            outputPath = j.value("outputPath",
                constants::getRecordingFilePath(message.sessionId, constants::DEFAULT_OUTPUT_FORMAT));

        } catch (const std::exception& e) {
            sendError(message.sessionId, std::string("Invalid config JSON: ") + e.what());
            return;
        }

        logger_->info("Agent", "Creating new recording session", message.sessionId);
        // Opened at session creation, not at start success: the engine/OBS init lines need attribution too.
        logger_->setSessionContext(message.sessionId);

        auto context = std::make_shared<RecordingContext>(
            engine_,
            fileManager_,
            logger_);

        currentSession_ = std::make_shared<RecordingSession>(message.sessionId, context);
        currentSessionId_ = message.sessionId;

        bool success = false;
        try {
            success = currentSession_->start(config);
        } catch (const std::exception& e) {
            sendError(message.sessionId, std::string("Session start failed: ") + e.what());
            currentSession_.reset();
            currentSessionId_.clear();
            logger_->clearSessionContext();
            return;
        }

        if (!success) {
            sendError(message.sessionId, "Failed to start recording session");
            currentSession_.reset();
            currentSessionId_.clear();
            logger_->clearSessionContext();
            return;
        }

        logger_->info("Agent", "Recording session started successfully");
        const DisplayTopology displaysAtSessionStart = currentDisplayTopology();
        if (displaysAtStart_.count > 0 && displaysAtSessionStart.count >= 0 &&
            displaysAtSessionStart.count < displaysAtStart_.count) {
            logger_->warning("Agent",
                "Session starting with " + std::to_string(displaysAtSessionStart.count) +
                    " display(s); previous session last saw " + std::to_string(displaysAtStart_.count) +
                    ". The canvas is fixed at start, so displays attached mid-call will be scaled down");
        }
        displaysAtStart_ = displaysAtSessionStart;
        lastSeenTopology_ = displaysAtStart_.signature;
        wasLocked_ = workstationLocked();
        captureRetriesLeft_ = CAPTURE_REBUILD_RETRIES;
#ifdef __APPLE__
        macos::showRecordingIndicator();
#endif

        if (config.maxDurationSeconds > 0) {
            recordingDeadline_.store(steadyNowNs() +
                                     (static_cast<int64_t>(config.maxDurationSeconds) * 1000000000LL));
        } else {
            recordingDeadline_.store(0);
        }

        std::string actualOutputPath = currentSession_->getMetadata().filePath;
        currentManifestPath_ = journal::manifestPathForRecording(actualOutputPath);

        try {
            json j = json::parse(message.payload);

            json manifest;
            manifest["sessionId"] = message.sessionId;
            manifest["filePath"] = actualOutputPath;
            manifest["createdAt"] = std::chrono::system_clock::now().time_since_epoch().count();

            manifest["metadata"] = json::array({j.value("metadata", json::object())});

            const int64_t startTime = j.value("startTime", static_cast<int64_t>(0));
            journal::setStartTime(manifest,
                startTime > 0 ? startTime : journal::nowEpochMillis());

            if (j.contains("s3SignedUploadUrl")) {
                manifest["s3SignedUploadUrl"] = j["s3SignedUploadUrl"];
            }
            manifest["s3ManifestUploadUrl"] = j.value("s3ManifestUploadUrl", "");

            journal::setLiveState(manifest, journal::STATE_RECORDING);
            if (journal::atomicWrite(currentManifestPath_, manifest)) {
                logger_->info("Agent", "Manifest file created: " + currentManifestPath_);
            } else {
                logger_->warning("Agent", "Failed to create manifest file: " + currentManifestPath_);
            }
        } catch (const std::exception& e) {
            logger_->warning("Agent", std::string("Failed to create manifest: ") + e.what());
        }

        IPCMessage response;
        response.type = MessageType::RESPONSE;
        response.action = ipc::protocol::ACTION_RECORDING_STARTED;
        response.requestId = activeRequestId_;
        response.sessionId = message.sessionId;
        json responseData;
        responseData["outputPath"] = actualOutputPath;
        response.payload = responseData.dump();
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    void handleStopRecording(const IPCMessage& message) {
        std::scoped_lock lock(sessionMutex_);

        if (!currentSession_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_NO_ACTIVE_SESSION,
                "No active recording session");
            return;
        }

        if (message.sessionId != currentSessionId_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_SESSION_CONFLICT,
                "Session ID mismatch: requested '" + message.sessionId +
                    "' but currently recording '" + currentSessionId_ + "'");
            return;
        }

        std::string endReason = journal::END_CLIENT_STOP;
        if (!message.payload.empty()) {
            const auto p = json::parse(message.payload, nullptr, false);
            if (p.is_object()) {
                endReason = p.value("reason", journal::END_CLIENT_STOP);
            }
        }

        finalizeAndUploadLocked(/*sendStopResponse=*/true, endReason);
    }

    void handleAppendMetadata(const IPCMessage& message) {
        std::scoped_lock lock(sessionMutex_);

        if (!currentSession_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_NO_ACTIVE_SESSION,
                "No active recording session");
            return;
        }

        if (message.sessionId != currentSessionId_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_SESSION_CONFLICT,
                "Session ID mismatch: requested '" + message.sessionId +
                    "' but currently recording '" + currentSessionId_ + "'");
            return;
        }

        json entry = json::object();
        if (!message.payload.empty()) {
            const auto p = json::parse(message.payload, nullptr, false);
            if (p.is_object()) {
                entry = p.value("metadata", json::object());
            }
        }

        const bool appended = journal::update(currentManifestPath_, [&entry](json& m) {
            journal::appendMetadata(m, entry);
            return true;
        });
        if (!appended) {
            sendErrorCode(message.sessionId, "append_failed",
                "Failed to append start request to manifest");
            return;
        }

        IPCMessage response;
        response.type = MessageType::RESPONSE;
        response.action = ipc::protocol::ACTION_METADATA_APPENDED;
        response.requestId = activeRequestId_;
        response.sessionId = message.sessionId;
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    // sendStopResponse only for client stops (the only ones with a waiter). Caller MUST hold sessionMutex_.
    void finalizeAndUploadLocked(bool sendStopResponse, const std::string& endReason) {
        if (!currentSession_) {
            return;
        }

        const std::string sessionId = currentSessionId_;
        const std::string manifestPath = currentManifestPath_;
        recordingDeadline_.store(0);

        logger_->info("Agent", "Stopping recording session (" + endReason + ")", sessionId);

        try {
            currentSession_->stop();
        } catch (const std::exception& e) {
            if (sendStopResponse) {
                sendError(sessionId, std::string("Stop failed: ") + e.what());
            } else {
                logger_->error("Agent", "Auto-stop failed: " + std::string(e.what()), sessionId);
            }
            journal::update(manifestPath, [&](json& m) {
                journal::setLastError(m, "stop_failed", e.what());
                return journal::finalize(m, journal::STATE_FAILED, journal::END_ERROR,
                    -1, 0, /*partial=*/true);
            });
            if (!sendStopResponse) {
                sendRecordingEndedLocked(sessionId, journal::STATE_FAILED, journal::END_ERROR,
                    "", 0, -1);
            }
            // Tear down regardless: a session we cannot stop must not block future starts forever.
            teardownSessionLocked();
            return;
        }

        auto metadata = currentSession_->getMetadata();

        // A stop that "succeeded" can still finalize dirty: the muxer reports write errors via the stop code.
        const int stopCode = engine_->lastStopCode();
        const bool dirty = stopCode != stop_reason::CODE_SUCCESS;
        const auto classification = stop_reason::classify(stopCode);
        const std::string finalState = dirty ? journal::STATE_FAILED : journal::STATE_STOPPED;
        const std::string finalReason = dirty ? classification.endReason : endReason;
        journal::update(manifestPath, [&](json& m) {
            if (dirty) {
                journal::setLastError(m, classification.errorCode, classification.message);
            }
            return journal::finalize(m, finalState, finalReason, metadata.duration.count(),
                metadata.fileSizeBytes,
                dirty || journal::partialFor(finalReason));
        });

        if (sendStopResponse) {
            IPCMessage response;
            response.type = MessageType::RESPONSE;
            response.action = ipc::protocol::ACTION_RECORDING_STOPPED;
            response.requestId = activeRequestId_;
            response.sessionId = sessionId;
            json responseData;
            responseData["outputPath"] = metadata.filePath;
            responseData["duration"] = metadata.duration.count();
            responseData["fileSizeBytes"] = metadata.fileSizeBytes;
            responseData["state"] = RecordingContext::stateToString(metadata.state);
            response.payload = responseData.dump();
            response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            pipeClient_->sendMessage(response);
        } else {
            sendRecordingEndedLocked(sessionId, finalState, finalReason,
                metadata.filePath, metadata.fileSizeBytes, metadata.duration.count());
        }

        if (metadata.state == RecordingState::STOPPED && !metadata.filePath.empty()) {
            sendUploadRequestLocked(sessionId, metadata.filePath, metadata.fileSizeBytes,
                metadata.duration.count());
        }

        teardownSessionLocked();

        logger_->info("Agent", "Recording session stopped and destroyed", sessionId);
    }

    // Self-stops have no waiter, but the daemon must hear about them or its watchdog keeps answering "active".
    // Unsolicited, so requestId stays empty and never satisfies a pending wait.
    void sendRecordingEndedLocked(const std::string& sessionId, const std::string& state,
        const std::string& endReason, const std::string& outputPath,
        uint64_t fileSizeBytes, int64_t durationMs) {
        IPCMessage ended;
        ended.type = MessageType::RESPONSE;
        ended.action = ipc::protocol::ACTION_RECORDING_ENDED;
        ended.sessionId = sessionId;
        json p;
        p["state"] = state;
        p["endReason"] = endReason;
        p["outputPath"] = outputPath;
        p["fileSizeBytes"] = fileSizeBytes;
        p["durationMs"] = durationMs;
        ended.payload = p.dump();
        ended.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(ended);
    }

    // Caller MUST hold sessionMutex_.
    void sendUploadRequestLocked(const std::string& sessionId, const std::string& filePath,
        uint64_t fileSizeBytes, int64_t durationMs) {
        logger_->info("Agent", "Sending upload request to service for: " + filePath);

        IPCMessage uploadRequest;
        uploadRequest.type = MessageType::UPLOAD_REQUEST;
        uploadRequest.action = "upload_file";
        uploadRequest.sessionId = sessionId;

        json uploadData;
        uploadData["filePath"] = filePath;
        uploadData["fileSizeBytes"] = fileSizeBytes;
        uploadData["duration"] = durationMs;

        const std::string manifestPath = journal::manifestPathForRecording(filePath);
        uploadData["manifestPath"] = manifestPath;
        const json manifest = journal::load(manifestPath);
        if (manifest.is_object()) {
            if (manifest.contains("s3SignedUploadUrl")) {
                uploadData["s3SignedUploadUrl"] = manifest["s3SignedUploadUrl"];
            }
            if (manifest.contains("s3ManifestUploadUrl")) {
                uploadData["s3ManifestUploadUrl"] = manifest["s3ManifestUploadUrl"];
            }
            logger_->debug("Agent", "Loaded upload URLs from manifest: " + manifestPath);
        } else {
            logger_->warning("Agent", "Manifest unreadable: " + manifestPath + " (will use config)");
        }

        uploadRequest.payload = uploadData.dump();
        uploadRequest.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(uploadRequest);
    }

#ifdef _WIN32
    static int processThreadCount() {
        int threads = 0;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            const DWORD pid = GetCurrentProcessId();
            if (Thread32First(snapshot, &entry) != 0) {
                do {
                    if (entry.th32OwnerProcessID == pid) {
                        ++threads;
                    }
                } while (Thread32Next(snapshot, &entry) != 0);
            }
            CloseHandle(snapshot);
        }
        return threads;
    }

    static ULONG CALLBACK onPowerEvent(void* context, ULONG type, void*) {
        auto* self = static_cast<RecordingAgent*>(context);
        if (type == PBT_APMSUSPEND || type == PBT_APMRESUMEAUTOMATIC ||
            type == PBT_APMRESUMESUSPEND) {
            std::unique_lock<std::mutex> lock(self->sessionMutex_, std::try_to_lock);
            if (lock.owns_lock() && !self->currentSession_) {
                self->engine_->trimGraphicsMemory();
                self->logger_->info("Agent",
                    std::string("Power transition (") +
                        (type == PBT_APMSUSPEND ? "suspend" : "resume") +
                        ") while idle; vitals after trim: " + processVitals());
            }
        }
        return 0;
    }

    static uint64_t processPrivateBytes() {
        PROCESS_MEMORY_COUNTERS pmc = {};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        return pmc.PagefileUsage;
    }

    static std::string processVitals() {
        PROCESS_MEMORY_COUNTERS pmc = {};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        return "threads=" + std::to_string(processThreadCount()) +
               " handles=" + std::to_string(handles) +
               " workingSetMB=" + std::to_string(pmc.WorkingSetSize / (1024 * 1024)) +
               " privateMB=" + std::to_string(pmc.PagefileUsage / (1024 * 1024));
    }
#endif

    // Caller MUST hold sessionMutex_.
    void teardownSessionLocked() {
#ifdef _WIN32
        logger_->info("Agent", "Session-end process vitals: " + processVitals());
#endif
        recordingDeadline_.store(0);
        currentSession_.reset();
        currentSessionId_.clear();
        currentManifestPath_.clear();
        displaysAtStart_ = {};
        lastSeenTopology_.clear();
        captureRetriesLeft_ = 0;
        wasLocked_ = false;
        zeroFrameTicks_ = 0;
        logger_->clearSessionContext();
#ifdef __APPLE__
        macos::hideRecordingIndicator();
#endif
    }

    struct DisplayTopology {
        int count = -1;
        std::string signature;
    };

    static DisplayTopology currentDisplayTopology() {
#ifdef __APPLE__
        const auto monitors = macos::detectMonitors();
        std::string sig;
        for (const auto& m : monitors) {
            sig += m.name + '|' + std::to_string(m.x) + ',' + std::to_string(m.y) + ',' +
                   std::to_string(m.width) + 'x' + std::to_string(m.height) + ',' +
                   std::to_string(m.dpiScaleX) + (m.isPrimary ? "*;" : ";");
        }
        return {static_cast<int>(monitors.size()), std::move(sig)};
#else
        struct Enumerated {
            std::vector<std::string> monitors;
        };
        Enumerated enumerated;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
                MONITORINFOEXA info;
                info.cbSize = sizeof(info);
                if (GetMonitorInfoA(monitor, &info) != 0) {
                    reinterpret_cast<Enumerated*>(param)->monitors.emplace_back(
                        std::string(info.szDevice) + '|' +
                        std::to_string(info.rcMonitor.left) + ',' +
                        std::to_string(info.rcMonitor.top) + ',' +
                        std::to_string(info.rcMonitor.right - info.rcMonitor.left) + 'x' +
                        std::to_string(info.rcMonitor.bottom - info.rcMonitor.top) +
                        (((info.dwFlags & MONITORINFOF_PRIMARY) != 0) ? "*;" : ";"));
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&enumerated));
        std::sort(enumerated.monitors.begin(), enumerated.monitors.end());
        std::string sig;
        for (const auto& entry : enumerated.monitors) {
            sig += entry;
        }
        return {static_cast<int>(enumerated.monitors.size()), std::move(sig)};
#endif
    }

    static bool workstationLocked() {
#ifdef _WIN32
        const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (desktop == nullptr) {
            return true;
        }
        CloseDesktop(desktop);
        return false;
#else
        return false;
#endif
    }

    // Engine-reported death of a live recording. Caller MUST hold sessionMutex_.
    void failSessionLocked(const std::string& message) {
        const std::string sessionId = currentSessionId_;
        const std::string manifestPath = currentManifestPath_;
        recordingDeadline_.store(0);

        logger_->error("Agent", "Recording failed (" + sessionId + "): " + message);

        try {
            currentSession_->handleCaptureFailure(message);
        } catch (const std::exception& e) {
            logger_->error("Agent", std::string("FSM failure transition threw: ") + e.what());
        }
        // Finalize whatever the muxer wrote; idempotent if the output already stopped on its own.
        try {
            engine_->stop();
        } catch (const std::exception& e) {
            logger_->error("Agent", std::string("Engine stop after failure threw: ") + e.what());
        }

        const int stopCode = engine_->lastStopCode();
        const auto classification = stop_reason::classify(stopCode);
        const std::string endReason = stopCode != stop_reason::CODE_SUCCESS
                                          ? classification.endReason
                                          : journal::END_ERROR;
        const std::string errorCode = (classification.errorCode != nullptr &&
                                          *classification.errorCode != '\0')
                                          ? classification.errorCode
                                          : "engine_error";

        const auto metadata = currentSession_->getMetadata();
        std::error_code ec;
        uint64_t fileSize = std::filesystem::file_size(metadata.filePath, ec);
        if (ec) {
            fileSize = 0;
        }
        const int64_t durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - metadata.startTime)
                                       .count();

        journal::update(manifestPath, [&](json& m) {
            journal::setLastError(m, errorCode, message);
            return journal::finalize(m, journal::STATE_FAILED, endReason, durationMs,
                fileSize, /*partial=*/true);
        });

        IPCMessage failed;
        failed.type = MessageType::ERROR_MSG;
        failed.action = ipc::protocol::ACTION_RECORDING_FAILED;
        failed.sessionId = sessionId;
        json p;
        p["endReason"] = endReason;
        p["message"] = message;
        p["outputPath"] = metadata.filePath;
        p["fileSizeBytes"] = fileSize;
        failed.payload = p.dump();
        failed.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(failed);

        // Partial beats nothing for compliance: upload what survived.
        if (!metadata.filePath.empty() && fileSize > 0) {
            sendUploadRequestLocked(sessionId, metadata.filePath, fileSize, durationMs);
        }

        teardownSessionLocked();
    }

    // Under sessionMutex_: the heartbeat auto-stop can reset currentSession_ (unlocked op = use-after-free).
    void handleSessionOp(const IPCMessage& message,
        const std::function<bool(RecordingSession&)>& op,
        const std::string& successAction,
        const std::string& failMessage,
        const char* journalState = nullptr) {
        std::scoped_lock lock(sessionMutex_);

        if (!currentSession_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_NO_ACTIVE_SESSION,
                "No active recording session");
            return;
        }
        if (message.sessionId != currentSessionId_) {
            sendErrorCode(message.sessionId, ipc::protocol::ERR_SESSION_CONFLICT,
                "Session ID mismatch: requested '" + message.sessionId +
                    "' but currently recording '" + currentSessionId_ + "'");
            return;
        }
        if (!op(*currentSession_)) {
            sendError(message.sessionId, failMessage);
            return;
        }
        if (journalState != nullptr && !currentManifestPath_.empty()) {
            journal::update(currentManifestPath_, [&](json& m) {
                journal::setLiveState(m, journalState);
                return true;
            });
        }
        sendSuccess(successAction);
    }

    void handlePauseRecording(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.pause(); }, "recording_paused", "Failed to pause recording", journal::STATE_PAUSED);
    }

    void handleResumeRecording(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.resume(); }, "recording_resumed", "Failed to resume recording", journal::STATE_RECORDING);
    }

    void handlePauseAudio(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.pauseAudio(); }, "audio_paused", "Failed to pause audio");
    }

    void handlePauseVideo(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.pauseVideo(); }, "video_paused", "Failed to pause video");
    }

    void handleResumeAudio(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.resumeAudio(); }, "audio_resumed", "Failed to resume audio");
    }

    void handleResumeVideo(const IPCMessage& message) {
        handleSessionOp(message, [](RecordingSession& s) { return s.resumeVideo(); }, "video_resumed", "Failed to resume video");
    }

    void handleGetStatus() {
        std::scoped_lock lock(sessionMutex_);

        IPCMessage response;
        response.type = MessageType::RESPONSE;
        response.action = "status";
        response.requestId = activeRequestId_;

        json j;
        if (currentSession_) {
            auto metadata = currentSession_->getMetadata();
            j["isRecording"] = true;
            j["state"] = RecordingContext::stateToString(metadata.state);
            j["sessionId"] = currentSessionId_;
            j["filePath"] = metadata.filePath;
            j["fileSizeBytes"] = metadata.fileSizeBytes;
            j["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - metadata.startTime)
                                  .count();
            if (!metadata.errorMessage.empty()) {
                j["lastError"] = metadata.errorMessage;
            }
        } else {
            j["isRecording"] = false;
            j["state"] = "Idle";
            j["sessionId"] = "";
        }
        j["connected"] = pipeClient_->isConnected();
        j["running"] = running_.load();

        const std::string stateKey = j["state"].get<std::string>() + "|" + currentSessionId_;
        std::string line = "Status: state=" + j["state"].get<std::string>();
        if (currentSession_) {
            line += " session=" + currentSessionId_ +
                    " bytes=" + std::to_string(j["fileSizeBytes"].get<uint64_t>()) +
                    " durMs=" + std::to_string(j["durationMs"].get<int64_t>());
            if (j.contains("lastError")) {
                line += " lastError=" + j["lastError"].get<std::string>();
            }
        }
#ifdef _WIN32
        line += " | " + processVitals();
#endif
        if (stateKey != lastStatusKey_ || ++statusPollsSinceLog_ >= STATUS_LOG_EVERY_POLLS) {
            logger_->info("Agent", line);
            lastStatusKey_ = stateKey;
            statusPollsSinceLog_ = 0;
        } else {
            logger_->debug("Agent", line);
        }

        response.payload = j.dump();
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    void handleConfigure(const IPCMessage& message) {
        logger_->info("Agent", "Received configure request");

        if (!message.payload.empty()) {
            try {
                const json config = json::parse(message.payload);
                if (config.contains("logging") && config["logging"].is_object()) {
                    LogLevel level = logger_->getLevel();
                    if (logLevelFromString(config["logging"].value("level", ""), level)) {
                        if (level <= LogLevel::INFO) {
                            logger_->setLevel(level);
                            logger_->info("Agent",
                                std::string("Log level set to ") + logLevelToString(level));
                        } else {
                            logger_->info("Agent",
                                std::string("Log level set to ") + logLevelToString(level));
                            logger_->setLevel(level);
                        }
                    }
                }
            } catch (const std::exception& e) {
                sendError(message.sessionId, std::string("Invalid config JSON: ") + e.what());
                return;
            }
        }

        sendSuccess("configured");
    }

    void handleShutdown(const IPCMessage& message) {
        std::string endReason = journal::END_SHUTDOWN;
        const auto payload = nlohmann::json::parse(message.payload, nullptr, false);
        if (payload.is_object() &&
            payload.value(ipc::protocol::SHUTDOWN_REASON_KEY, std::string{}) ==
                journal::END_SESSION_DETACHED) {
            endReason = journal::END_SESSION_DETACHED;
        }
        logger_->info("Agent", "Received shutdown command (" + endReason + ")");
        // Finalize while the pipe is still up so the upload request goes out (Windows logoff path).
        {
            std::scoped_lock lock(sessionMutex_);
            finalizeAndUploadLocked(/*sendStopResponse=*/false, endReason);
        }
        running_ = false;
    }

    void heartbeatLoop() {
        while (running_) {
            {
                std::unique_lock<std::mutex> lock(shutdownMutex_);
                shutdownCV_.wait_for(lock,
                    std::chrono::seconds(constants::AGENT_HEARTBEAT_INTERVAL_SEC),
                    [this] { return !running_.load(); });
            }

            if (!running_) {
                break;
            }

            IPCMessage heartbeat;
            heartbeat.type = MessageType::HEARTBEAT;
            heartbeat.action = "heartbeat";
            heartbeat.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

            if (!pipeClient_->sendMessage(heartbeat)) {
                logger_->error("Agent", "Failed to send heartbeat");
            }
        }
    }

    void monitorLoop() {
        int64_t lastSteadyNs = steadyNowNs();
        int64_t lastSystemNs = journal::nowNs();
        while (running_) {
            {
                std::unique_lock<std::mutex> lock(shutdownMutex_);
                std::chrono::nanoseconds waitFor = std::chrono::seconds(AGENT_MONITOR_INTERVAL_SEC);
                if (const int64_t deadline = recordingDeadline_.load(); deadline != 0) {
                    waitFor = std::min(waitFor,
                        std::chrono::nanoseconds(std::max<int64_t>(0, deadline - steadyNowNs())));
                }
                shutdownCV_.wait_for(lock, waitFor, [this] { return !running_.load(); });
            }
            if (!running_) {
                break;
            }

            // Sleep detection by clock divergence; nothing is captured while asleep, so stopping loses nothing.
            const int64_t steadyNs = steadyNowNs();
            const int64_t systemNs = journal::nowNs();
            const int64_t gapNs = (systemNs - lastSystemNs) - (steadyNs - lastSteadyNs);
            lastSteadyNs = steadyNs;
            lastSystemNs = systemNs;
            if (gapNs > SLEEP_GAP_THRESHOLD_NS) {
                std::scoped_lock lock(sessionMutex_);
                if (currentSession_) {
                    logger_->warning("Agent",
                        "System slept ~" + std::to_string(gapNs / 1000000000LL) +
                            "s mid-recording; stopping " + currentSessionId_);
                    finalizeAndUploadLocked(/*sendStopResponse=*/false,
                        journal::END_SYSTEM_SLEEP);
                }
                continue;
            }

            if (pendingEngineFailure_.exchange(false)) {
                std::string message;
                {
                    std::scoped_lock lock(failureMutex_);
                    message = pendingFailureMessage_;
                }
                std::scoped_lock lock(sessionMutex_);
                if (currentSession_) {
                    failSessionLocked(message);
                }
                continue;
            }

            std::scoped_lock lock(sessionMutex_);
            if (!currentSession_) {
                zeroFrameTicks_ = 0;
#ifdef _WIN32
                if (processThreadCount() > IDLE_RESTART_THREAD_THRESHOLD ||
                    processPrivateBytes() > IDLE_RESTART_PRIVATE_BYTES) {
                    logger_->warning("Agent",
                        "Idle with leaked encoder-runtime resources (" + processVitals() +
                            "); exiting for a clean relaunch by the service");
                    running_ = false;
                    shutdownCV_.notify_all();
                }
#endif
                continue;
            }

            if (const int64_t deadline = recordingDeadline_.load();
                deadline != 0 && steadyNowNs() >= deadline) {
                logger_->warning("Agent",
                    "Max recording duration reached; auto-stopping session: " + currentSessionId_);
                finalizeAndUploadLocked(/*sendStopResponse=*/false, journal::END_MAX_DURATION);
                continue;
            }

            if (engine_->isRunning()) {
                if (engine_->liveEncodedFrames() == 0) {
                    if (++zeroFrameTicks_ >= ZERO_FRAME_FAIL_TICKS) {
                        zeroFrameTicks_ = 0;
                        failSessionLocked(
                            "Encoder produced no frames for " +
                            std::to_string(ZERO_FRAME_FAIL_TICKS * AGENT_MONITOR_INTERVAL_SEC) +
                            "s; capture/encode pipeline is dead");
                        continue;
                    }
                } else {
                    zeroFrameTicks_ = 0;
                }
            }

#ifdef __APPLE__
            // Catches survivable TCC revocation before it turns into silently black frames.
            if (!macos::screenCaptureAccessPreflight()) {
                logger_->error("Agent",
                    "Screen recording permission revoked mid-recording; stopping " +
                        currentSessionId_);
                finalizeAndUploadLocked(/*sendStopResponse=*/false,
                    journal::END_PERMISSION_REVOKED);
                continue;
            }
#endif
            std::error_code ec;
            const auto space = std::filesystem::space(constants::recordingsDir(), ec);
            if (!ec && space.available < MIN_FREE_DISK_BYTES) {
                logger_->error("Agent",
                    "Free disk below floor (" + std::to_string(space.available) +
                        " bytes); stopping " + currentSessionId_);
                finalizeAndUploadLocked(/*sendStopResponse=*/false, journal::END_DISK_FULL);
                continue;
            }

            // Hot-plug must never interrupt the recording: retile live, the fixed canvas/encoder never change.
            if (const DisplayTopology displaysNow = currentDisplayTopology();
                displaysAtStart_.count >= 0 && displaysNow.count >= 0) {
                const bool lockedNow = workstationLocked();
                const bool justUnlocked = wasLocked_ && !lockedNow;
                wasLocked_ = lockedNow;
                if (displaysNow.signature != displaysAtStart_.signature) {
                    if (displaysNow.signature == lastSeenTopology_) {
                        logger_->warning("Agent",
                            "Display topology changed mid-recording (" +
                                std::to_string(displaysAtStart_.count) + " -> " +
                                std::to_string(displaysNow.count) + " displays); retiling, recording continues on " +
                                currentSessionId_);
                        float retileScale = 1.0f;
                        const bool retiled = engine_->retileDisplays(&retileScale);
                        if (retiled && retileScale < 0.75f) {
                            logger_->warning("Agent",
                                "Composite squeezed to " + std::to_string(retileScale) +
                                    " of the start-of-call canvas; expect reduced sharpness on " +
                                    currentSessionId_);
                        }
                        journal::update(currentManifestPath_, [&](json& m) {
                            m["journal"]["displayChange"] = {
                                {"atStart", displaysAtStart_.count},
                                {"now", displaysNow.count},
                                {"retiled", retiled},
                                {"retileScale", retileScale},
                                {"at", journal::nowNs()},
                            };
                            return true;
                        });
                        displaysAtStart_ = displaysNow;
                        captureRetriesLeft_ = CAPTURE_REBUILD_RETRIES;
                    }
                } else if (justUnlocked) {
                    logger_->warning("Agent",
                        "Workstation unlocked; rebuilding capture sources on " + currentSessionId_);
                    engine_->retileDisplays();
                    captureRetriesLeft_ = CAPTURE_REBUILD_RETRIES;
                } else if (!lockedNow) {
                    if (engine_->captureSourcesHealthy()) {
                        captureRetriesLeft_ = CAPTURE_REBUILD_RETRIES;
                    } else if (captureRetriesLeft_ > 0) {
                        --captureRetriesLeft_;
                        logger_->warning("Agent",
                            "Dead capture source after display change; rebuilding (" +
                                std::to_string(captureRetriesLeft_) + " retries left) on " +
                                currentSessionId_);
                        engine_->retileDisplays();
                    }
                }
                lastSeenTopology_ = displaysNow.signature;
            }
        }
    }

    static int64_t steadyNowNs() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    void sendError(const std::string& sessionId, const std::string& error) {
        IPCMessage response;
        response.type = MessageType::ERROR_MSG;
        response.action = "error";
        response.requestId = activeRequestId_;
        response.sessionId = sessionId;
        response.payload = error;
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    void sendSuccess(const std::string& action) {
        IPCMessage response;
        response.type = MessageType::RESPONSE;
        response.action = action;
        response.requestId = activeRequestId_;
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    // Structured {code, message, activeSessionId?}; the daemon maps codes to HTTP. Caller MUST hold sessionMutex_.
    void sendErrorCode(const std::string& sessionId, const std::string& code,
        const std::string& message) {
        IPCMessage response;
        response.type = MessageType::ERROR_MSG;
        response.action = "error";
        response.requestId = activeRequestId_;
        response.sessionId = sessionId;
        json payload;
        payload["code"] = code;
        payload["message"] = message;
        if (!currentSessionId_.empty()) {
            payload["activeSessionId"] = currentSessionId_;
        }
        response.payload = payload.dump();
        response.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        pipeClient_->sendMessage(response);
    }

    std::shared_ptr<LogService> logger_;
    std::shared_ptr<RecordingEngine> engine_;
    std::shared_ptr<FileManager> fileManager_;

    // Per-session state, guarded by sessionMutex_: the loops race the pipe reader's start/stop handling.
    std::shared_ptr<RecordingSession> currentSession_;
    std::string currentSessionId_;
    std::string currentManifestPath_;
    std::mutex sessionMutex_;

    // Set by the engine error callback (OBS threads); drained by monitorLoop().
    std::atomic<bool> pendingEngineFailure_{false};
    std::mutex failureMutex_; // guards pendingFailureMessage_
    std::string pendingFailureMessage_;

    std::string activeRequestId_; // command being dispatched; pipe reader thread only

    // Max-duration auto-stop deadline as steady_clock ns since epoch; 0 = disabled.
    std::atomic<int64_t> recordingDeadline_{0};

    // Topology at session start; count -1 = no session. Guarded by sessionMutex_.
    DisplayTopology displaysAtStart_;
    std::string lastSeenTopology_;
    static constexpr int CAPTURE_REBUILD_RETRIES = 5;
    int captureRetriesLeft_ = 0;
    bool wasLocked_ = false;
    int zeroFrameTicks_ = 0;
    std::string lastStatusKey_;
    int statusPollsSinceLog_ = 0;

    std::unique_ptr<ipc::IIpcClient> pipeClient_;
    std::thread heartbeatThread_;
    std::thread monitorThread_;
#ifdef _WIN32
    HPOWERNOTIFY powerNotify_ = nullptr;
#endif
    std::atomic<bool> running_;
    std::mutex shutdownMutex_; // guards running_ transitions for shutdownCV_
    std::condition_variable shutdownCV_;

#ifdef __APPLE__
    bool launchedWithScreenPermission_ = false;
    int deniedSeconds_ = 0; // run()-loop seconds spent without permission
#endif
};

}
