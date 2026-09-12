#pragma once

#include <string>
#include <chrono>
#include <map>
#include <memory>
#include <cstdint>
#include <vector>

namespace screenrec {

using SessionId = std::string;
using Timestamp = std::chrono::system_clock::time_point;
using Duration = std::chrono::milliseconds;

enum class RecordingState : uint8_t {
    IDLE,
    RECORDING,
    PAUSED,
    STOPPING,
    STOPPED,
    ERR
};

enum class LogLevel : uint8_t {
    DEBUG,
    INFO,
    WARNING,
    ERR,
    CRITICAL
};

[[nodiscard]] inline bool logLevelFromString(const std::string& text, LogLevel& out) {
    if (text == "debug") {
        out = LogLevel::DEBUG;
    } else if (text == "info") {
        out = LogLevel::INFO;
    } else if (text == "warning") {
        out = LogLevel::WARNING;
    } else if (text == "error") {
        out = LogLevel::ERR;
    } else if (text == "critical") {
        out = LogLevel::CRITICAL;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] inline const char* logLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return "debug";
    case LogLevel::WARNING: return "warning";
    case LogLevel::ERR: return "error";
    case LogLevel::CRITICAL: return "critical";
    case LogLevel::INFO:
    default: return "info";
    }
}

enum class UploadStatus : uint8_t {
    PENDING,
    IN_PROGRESS,
    SUCCESS,
    FAILED,
    RETRYING,
    EXPIRED // pre-signed URL lapsed: terminal, session parked in error/
};

enum class PauseState : uint8_t {
    NONE,
    AUDIO_ONLY,
    VIDEO_ONLY,
    BOTH
};

// Defaults mirror config/config.json — keep in sync so a missing config behaves like a fresh install.
struct VideoConfig {
    // PRD default preset "Low": 720p @ 5fps; h264 because HEVC is unreliable in browsers.
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 5;
    uint32_t bitrate = 500000; // bits/sec; target for CBR/VBR
    uint32_t maxBitrate = 0;   // bits/sec; VBR ceiling (0 = auto: 2x bitrate)
    std::string codec = "h264";
    std::string preset = "slower";
    // "CBR" = hard size cap; "VBR" = capped variable bitrate; "CRF"/other = unbounded size.
    std::string rateControl = "CRF";
    uint32_t crf = 27;
    uint32_t keyintSec = 30; // keyframe interval (s); up to this much tail loss on a hard crash
    std::string encoder = "auto";
    std::string layout = "equalTiles";
};

struct AudioConfig {
    bool enabled = true;
    uint32_t sampleRate = 44100;
    uint32_t channels = 1;
    uint32_t bitrate = 32000; // AAC total (not per-channel); keeps mixed speech clean
    // Applies to BOTH mic and system audio: "rnnoise", "speex", "none".
    std::string noiseSuppression = "none";
};

struct RecordingConfig {
    VideoConfig video;
    AudioConfig audio;
    uint32_t maxDurationSeconds = 3600;
    std::string outputFormat = "mp4";
};

struct StorageConfig {
    std::string outputDirectory = "./recordings";
    uint64_t maxDiskUsageGB = 10;
    bool cleanupOnUpload = true;
    // Age-based local deletion (PRD: uploaded-or-7-days). 0 disables.
    uint32_t retentionDays = 7;
};

struct UploadConfig {
    // Retry ladder: attemptsPerRound tries with exponential backoff, then wait for the next round.
    uint32_t attemptsPerRound = 3;
    uint32_t retryDelaySeconds = 5;
    // Round cadence; 0 = only rescan on daemon start.
    uint32_t pendingRescanSeconds = 600;
    uint32_t uploadThreads = 2;
    uint32_t connectTimeoutSeconds = 30;
    uint32_t lowSpeedLimitBytesPerSec = 1000;
    uint32_t lowSpeedTimeSeconds = 60;
    uint32_t maxUploadSpeedBytesPerSec = 512 * 1024; // 4 Mbps; 0 = unlimited
    std::string urlsEndpoint;
};

struct LoggingConfig {
    LogLevel level = LogLevel::INFO;
    std::string logDirectory = "./logs";
    // Log-filename tag; set per process, never from config.json (both processes share the file).
    std::string processTag = "service";
    bool enableFileLogging = true;
    // Roll to a new timestamped file at this size (config key: maxFileSizeMB).
    size_t maxFileSizeBytes = 10UL * 1024 * 1024;
    uint32_t maxFiles = 5;
    bool enableLokiReporting = true;
    std::string lokiEndpoint;
    std::string lokiApiKey;
    std::string lokiSecretKey;
    // Fetched with the configure request's JWT to fill the three loki values; empty disables it.
    std::string credentialsEndpoint;
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::vector<uint16_t> ports{8080}; // bind candidates; first free wins
    bool enableAuth = false;
    bool enableHttps = true;
    std::string sslCertPath = "certs/localhost.crt";
    std::string sslKeyPath = "certs/localhost.key";
    // X-API-Key JWT verification key; requests fail closed when enableAuth is on and it is absent.
    std::string jwtPublicKeyPath = "certs/public.pem";
    std::map<std::string, std::string> jwtPublicKeysByKit;
    std::string jwtIssuer = "3clogic"; // "" = skip the iss check (signature-only)
    // Allowed browser Origins ("*" = any, "null" = file://); anything else gets 403 and no CORS.
    std::vector<std::string> allowedOrigins;
    // HTTP worker threads; clamped to [2, 64] when loaded from config.
    uint32_t threadPoolSize = 8;
};

struct CallCenterInfo {
    std::string ccId;
    std::string agentId;
};

struct Configuration {
    RecordingConfig recording;
    StorageConfig storage;
    UploadConfig upload;
    LoggingConfig logging;
    CallCenterInfo callcenterInfo;
    ServerConfig server;
};

struct RecordingMetadata {
    SessionId sessionId;
    Timestamp startTime;
    Timestamp endTime;
    Duration duration{};
    VideoConfig videoConfig;
    AudioConfig audioConfig;
    std::string filePath;
    uint64_t fileSizeBytes = 0;
    RecordingState state = RecordingState::IDLE;
    std::string errorMessage;
};

struct MonitorInfo {
    int index;
    std::string name;
    int x, y;
    int width, height;
    bool isPrimary;
    float dpiScaleX = 1.0f;
    float dpiScaleY = 1.0f;
    // Scene-item scale for tiling under mixed-DPI backing scales (macOS); 1.0 on Windows.
    float renderScale = 1.0f;
};

}
