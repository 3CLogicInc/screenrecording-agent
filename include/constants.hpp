#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "version.hpp"

namespace screenrec::constants {
constexpr auto APP_NAME = "ScreenRecordingService";
constexpr auto APP_VERSION = SCREENREC_APP_VERSION;
constexpr auto API_VERSION = "v1";
constexpr auto APP_VENDOR = "ScreenRecorder";
#ifdef _WIN32
constexpr auto SERVICE_NAME = L"ScreenRecordingService";
constexpr auto SERVICE_DISPLAY_NAME = L"Screen Recording Service";
constexpr auto SERVICE_DESCRIPTION =
    L"Enterprise screen recording service with session management";
// Named-pipe name; NamedPipeServer/Client prepend "\\.\pipe\".
constexpr auto IPC_ENDPOINT = "ScreenRecordingService";
constexpr auto BASE_DATA_DIR = "C:\\ProgramData\\ScreenRecorder";
constexpr auto LOGS_DIR = R"(C:\ProgramData\ScreenRecorder\logs)";
constexpr auto RECORDINGS_DIR = R"(C:\ProgramData\ScreenRecorder\recordings)";
constexpr auto CONFIG_DIR = R"(C:\ProgramData\ScreenRecorder\config)";
#elif defined(__APPLE__)
constexpr auto DAEMON_LABEL = "com.3clogic.screenrecorder.daemon";
constexpr auto AGENT_LABEL = "com.3clogic.screenrecorder.agent";
constexpr auto IPC_ENDPOINT = "/var/run/screenrecording/agent.sock";
constexpr auto BASE_DATA_DIR = "/Library/Application Support/ScreenRecorder";
constexpr auto LOGS_DIR = "/Library/Logs/ScreenRecorder";
constexpr auto RECORDINGS_DIR = "/Library/Application Support/ScreenRecorder/recordings";
constexpr auto CONFIG_DIR = "/Library/Application Support/ScreenRecorder/config";
#else
constexpr auto IPC_ENDPOINT = "/var/run/screenrecording/agent.sock";
constexpr auto BASE_DATA_DIR = "/var/lib/screenrecorder";
constexpr auto LOGS_DIR = "/var/lib/screenrecorder/logs";
constexpr auto RECORDINGS_DIR = "/var/lib/screenrecorder/recordings";
constexpr auto CONFIG_DIR = "/var/lib/screenrecorder/config";
#endif
constexpr uint32_t PIPE_CONNECT_TIMEOUT_MS = 10000;
constexpr auto DEFAULT_OUTPUT_FORMAT = "mp4";
constexpr auto DEFAULT_VIDEO_CODEC = "h264";
constexpr auto DEFAULT_VIDEO_PRESET = "medium";
constexpr uint32_t DEFAULT_VIDEO_WIDTH = 1920;
constexpr uint32_t DEFAULT_VIDEO_HEIGHT = 1080;
constexpr uint32_t DEFAULT_VIDEO_FPS = 30;
constexpr auto DEFAULT_VIDEO_RATE_CONTROL = "CRF";
constexpr uint32_t DEFAULT_VIDEO_CRF = 28;
constexpr uint32_t DEFAULT_VIDEO_BITRATE = 2500000;
constexpr uint32_t DEFAULT_VIDEO_MAX_BITRATE = 0;
constexpr uint32_t DEFAULT_VIDEO_KEYINT_SEC = 2;
constexpr auto DEFAULT_VIDEO_LAYOUT = "equalTiles";
constexpr bool DEFAULT_AUDIO_ENABLED = true;
constexpr uint32_t DEFAULT_AUDIO_SAMPLE_RATE = 44100;
constexpr uint32_t DEFAULT_AUDIO_CHANNELS = 2;
constexpr uint32_t DEFAULT_AUDIO_BITRATE = 128000;
constexpr uint32_t DEFAULT_MAX_DURATION_SECONDS = 3600;
constexpr uint32_t AGENT_HEARTBEAT_INTERVAL_SEC = 30;
constexpr uint32_t STORAGE_CLEANUP_AGE_DAYS = 30;
constexpr uint32_t API_HEARTBEAT_TIMEOUT_SEC = 180;

inline std::string envOrDefault(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : fallback;
}

inline std::string recordingsDir() {
    return envOrDefault("SCREENREC_RECORDINGS_DIR", RECORDINGS_DIR);
}

inline std::string logsDir() {
    return envOrDefault("SCREENREC_LOGS_DIR", LOGS_DIR);
}

inline std::string getRecordingFilePath(const std::string& sessionId,
    const std::string& format) {
    return (std::filesystem::path(recordingsDir()) / (sessionId + "." + format)).string();
}

}
