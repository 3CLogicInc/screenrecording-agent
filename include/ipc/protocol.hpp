#pragma once

#include <json.hpp>
#include <string>

// Daemon <-> agent IPC contract, N/N-1 compatible via protocolVersion plus tolerant parsing; bump PROTOCOL_VERSION when the contract changes shape.
namespace screenrec::ipc::protocol {

inline constexpr int PROTOCOL_VERSION = 1;

// ACTION_ prefix: bare names like READY collide with Windows SDK macros.
inline constexpr const char* ACTION_READY = "ready";
inline constexpr const char* ACTION_REGISTERED = "registered";
inline constexpr const char* ACTION_START_RECORDING = "start_recording";
inline constexpr const char* ACTION_STOP_RECORDING = "stop_recording";
inline constexpr const char* ACTION_CONFIGURE = "configure";
inline constexpr const char* ACTION_PAUSE_RECORDING = "pause_recording";
inline constexpr const char* ACTION_RESUME_RECORDING = "resume_recording";
inline constexpr const char* ACTION_PAUSE_AUDIO = "pause_audio";
inline constexpr const char* ACTION_PAUSE_VIDEO = "pause_video";
inline constexpr const char* ACTION_RESUME_AUDIO = "resume_audio";
inline constexpr const char* ACTION_RESUME_VIDEO = "resume_video";
inline constexpr const char* ACTION_GET_STATUS = "get_status";
inline constexpr const char* ACTION_SHUTDOWN = "shutdown";
inline constexpr const char* ACTION_APPEND_METADATA = "append_metadata";
inline constexpr const char* ACTION_METADATA_APPENDED = "metadata_appended";
inline constexpr const char* ACTION_RECORDING_STARTED = "recording_started";
inline constexpr const char* ACTION_RECORDING_STOPPED = "recording_stopped";
// Agent -> daemon, unsolicited: the recording died (payload: endReason, message, outputPath, fileSizeBytes).
inline constexpr const char* ACTION_RECORDING_FAILED = "recording_failed";
// Agent -> daemon, unsolicited (empty requestId): payload state, endReason, outputPath, fileSizeBytes, durationMs; an N-1 daemon ignores it and heals via the terminal-journal check instead.
inline constexpr const char* ACTION_RECORDING_ENDED = "recording_ended";

// Codes for agent ERROR_MSG payloads (JSON {code, message, activeSessionId?}); older agents send plain text, so the daemon still falls back to string matching.
inline constexpr const char* ERR_ALREADY_RECORDING = "already_recording";
inline constexpr const char* ERR_SESSION_CONFLICT = "session_conflict";
inline constexpr const char* ERR_NO_ACTIVE_SESSION = "no_active_session";

struct ReadyPayload {
    int protocolVersion = 1;
    std::string agentVersion;
    std::string platform;
    int monitorCount = -1;
    // Tri-state matching the daemon's cache: -1 unknown, 0 denied, 1 granted.
    int screenRecordingPermission = -1;

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json j;
        j["protocolVersion"] = protocolVersion;
        j["agentVersion"] = agentVersion;
        j["platform"] = platform;
        if (monitorCount >= 0) {
            j["monitorCount"] = monitorCount;
        }
        if (screenRecordingPermission >= 0) {
            j["screenRecordingPermission"] = screenRecordingPermission == 1;
        }
        return j;
    }

    // Tolerant by design: missing or malformed fields keep their defaults so an older/newer peer still registers.
    [[nodiscard]] static ReadyPayload fromJson(const nlohmann::json& j) {
        ReadyPayload p;
        p.protocolVersion = j.value("protocolVersion", 1);
        p.agentVersion = j.value("agentVersion", std::string{});
        p.platform = j.value("platform", std::string{});
        p.monitorCount = j.value("monitorCount", -1);
        if (j.contains("screenRecordingPermission") &&
            j["screenRecordingPermission"].is_boolean()) {
            p.screenRecordingPermission = j["screenRecordingPermission"].get<bool>() ? 1 : 0;
        }
        return p;
    }
};

}
