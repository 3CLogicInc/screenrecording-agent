#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <string>

// "journal" sub-object in <sessionId>_manifest.json: how a session ended. First terminal wins.
namespace screenrec::journal {

using json = nlohmann::json;

// API-visible vocabulary: never rename existing values, the backend and CTI client match on them.
inline constexpr const char* STATE_RECORDING = "recording";
inline constexpr const char* STATE_PAUSED = "paused";
inline constexpr const char* STATE_STOPPED = "stopped";
inline constexpr const char* STATE_FAILED = "failed";

inline constexpr const char* END_CLIENT_STOP = "client_stop";
inline constexpr const char* END_WATCHDOG_TIMEOUT = "watchdog_timeout";
inline constexpr const char* END_MAX_DURATION = "max_duration";
inline constexpr const char* END_AGENT_CRASH = "agent_crash";
inline constexpr const char* END_SHUTDOWN = "shutdown";
inline constexpr const char* END_PERMISSION_REVOKED = "permission_revoked";
inline constexpr const char* END_DISK_FULL = "disk_full";
inline constexpr const char* END_SYSTEM_SLEEP = "system_sleep";
inline constexpr const char* END_ERROR = "error";
inline constexpr const char* END_UNKNOWN = "unknown";

[[nodiscard]] inline std::string endReasonMessage(const std::string& endReason) {
    if (endReason == END_CLIENT_STOP) {
        return "Recording ended: stopped by the client";
    }
    if (endReason == END_WATCHDOG_TIMEOUT) {
        return "Recording ended: the client stopped sending heartbeats";
    }
    if (endReason == END_MAX_DURATION) {
        return "Recording ended: maximum recording duration reached";
    }
    if (endReason == END_SYSTEM_SLEEP) {
        return "Recording ended: the machine went to sleep";
    }
    if (endReason == END_AGENT_CRASH) {
        return "Recording ended: the capture agent crashed";
    }
    if (endReason == END_SHUTDOWN) {
        return "Recording ended: the machine shut down";
    }
    if (endReason == END_PERMISSION_REVOKED) {
        return "Recording ended: screen-recording permission was revoked";
    }
    if (endReason == END_DISK_FULL) {
        return "Recording ended: the disk ran out of space";
    }
    if (endReason == END_ERROR) {
        return "Recording ended: an internal error occurred";
    }
    return "Recording ended";
}

inline constexpr const char* UPLOAD_PENDING = "pending";
inline constexpr const char* UPLOAD_UPLOADED = "uploaded";
inline constexpr const char* UPLOAD_FAILED = "failed";
inline constexpr const char* UPLOAD_EXPIRED = "expired";

inline int64_t nowNs() {
    return std::chrono::system_clock::now().time_since_epoch().count();
}

inline int64_t nowEpochMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string manifestPathForRecording(const std::string& recordingFilePath) {
    const std::filesystem::path p(recordingFilePath);
    return (p.parent_path() / p.stem()).string() + "_manifest.json";
}

inline std::string sessionManifestPath(const std::string& recordingsRoot,
    const std::string& sessionId) {
    return (std::filesystem::path(recordingsRoot) / sessionId / (sessionId + "_manifest.json"))
        .string();
}

// Archived before cleanupOnUpload deletes the session dir, so status still answers after upload.
inline std::string archivedJournalPath(const std::string& recordingsRoot,
    const std::string& sessionId) {
    return (std::filesystem::path(recordingsRoot) / "journal" / (sessionId + ".json")).string();
}

// Never-uploaded sessions land here instead of being deleted, kept for storage.errorRetentionDays
// (default 30) and exempt from ordinary retention/quota eviction.
inline std::string errorDirPath(const std::string& recordingsRoot) {
    return (std::filesystem::path(recordingsRoot) / "error").string();
}

inline std::string errorSessionManifestPath(const std::string& recordingsRoot,
    const std::string& sessionId) {
    return (std::filesystem::path(recordingsRoot) / "error" / sessionId /
            (sessionId + "_manifest.json"))
        .string();
}

// Both names match the session-id regex; scans that fail to skip them delete these dirs.
[[nodiscard]] inline bool isReservedRootDir(const std::string& name) {
    return name == "journal" || name == "error";
}

inline json load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    json j = json::parse(in, nullptr, false);
    return j.is_object() ? j : json{};
}

// Temp + rename: no torn reads, a crash mid-write keeps the old manifest. Replaces on POSIX/MSVC.
inline bool atomicWrite(const std::string& path, const json& j) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << j.dump(2);
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// True when the file may not cover the intended span; the excluded four run the full stop path.
[[nodiscard]] inline bool partialFor(const std::string& endReason) {
    return !(endReason == END_CLIENT_STOP || endReason == END_MAX_DURATION ||
             endReason == END_WATCHDOG_TIMEOUT || endReason == END_SYSTEM_SLEEP);
}

[[nodiscard]] inline bool isTerminal(const json& manifest) {
    if (!manifest.is_object() || !manifest.contains("journal")) {
        return false;
    }
    const std::string state = manifest["journal"].value("state", "");
    return state == STATE_STOPPED || state == STATE_FAILED;
}

// First-wins like finalize(): a late resume must not revive an already-terminal journal.
inline void setLiveState(json& manifest, const std::string& state, int64_t atNs = nowNs()) {
    if (isTerminal(manifest)) {
        return;
    }
    manifest["journal"]["state"] = state;
    manifest["journal"]["updatedAt"] = atNs;
}

inline void setLastError(json& manifest, const std::string& code, const std::string& message,
    int64_t atNs = nowNs()) {
    manifest["journal"]["lastError"] = {{"code", code}, {"message", message}, {"at", atNs}};
    manifest["journal"]["updatedAt"] = atNs;
}

inline void setStartTime(json& manifest, int64_t epochMillis) {
    manifest["journal"]["startTime"] = epochMillis;
}

[[nodiscard]] inline int64_t startTimeEpochMillis(const json& manifest) {
    if (!manifest.is_object() || !manifest.contains("journal") ||
        !manifest["journal"].is_object()) {
        return 0;
    }
    const auto& j = manifest["journal"];
    if (!j.contains("startTime") || !j["startTime"].is_number_integer()) {
        return 0;
    }
    return j["startTime"].get<int64_t>();
}

inline void setUploadState(json& manifest, const std::string& uploadState,
    int64_t atNs = nowNs()) {
    manifest["journal"]["uploadState"] = uploadState;
    manifest["journal"]["updatedAt"] = atNs;
}

// Epoch ns of the move into error/; 0 when unstamped. Never throws (runs on the error-sweep thread).
[[nodiscard]] inline int64_t quarantinedAtNs(const json& manifest) {
    if (!manifest.is_object() || !manifest.contains("journal") ||
        !manifest["journal"].is_object()) {
        return 0;
    }
    const auto& j = manifest["journal"];
    if (!j.contains("quarantinedAt") || !j["quarantinedAt"].is_number_integer()) {
        return 0;
    }
    return j["quarantinedAt"].get<int64_t>();
}

inline void appendMetadata(json& manifest, const json& entry, int64_t atNs = nowNs()) {
    if (!manifest["metadata"].is_array()) {
        manifest["metadata"] = json::array();
    }
    manifest["metadata"].push_back(entry);
    manifest["journal"]["updatedAt"] = atNs;
}

// First terminal record wins: crash recovery must not overwrite a clean stop's endReason.
inline bool finalize(json& manifest, const std::string& state, const std::string& endReason,
    int64_t durationMs, uint64_t fileSizeBytes, bool partial,
    int64_t atNs = nowNs()) {
    if (isTerminal(manifest)) {
        return false;
    }
    auto& j = manifest["journal"];
    j["state"] = state;
    j["endReason"] = endReason;
    j["endedAt"] = atNs;
    j["updatedAt"] = atNs;
    if (durationMs >= 0) {
        j["durationMs"] = durationMs;
    }
    j["fileSizeBytes"] = fileSizeBytes;
    j["partial"] = partial;
    if (!j.contains("uploadState")) {
        j["uploadState"] = UPLOAD_PENDING;
    }
    return true;
}

template <typename Fn>
inline bool update(const std::string& path, const Fn& mutate) {
    json manifest = load(path);
    if (!manifest.is_object()) {
        return false;
    }
    if (!mutate(manifest)) {
        return false;
    }
    return atomicWrite(path, manifest);
}

// Agent manifests carry the recording stem, so an exact-name miss must scan the session dir.
inline std::string manifestPathInDir(const std::filesystem::path& dir) {
    const std::string suffix = "_manifest.json";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec;
        it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (load(it->path().string()).is_object()) {
                return it->path().string();
            }
        }
    }
    return {};
}

inline json loadManifestInDir(const std::filesystem::path& dir) {
    const std::string path = manifestPathInDir(dir);
    return path.empty() ? json{} : load(path);
}

struct Located {
    std::string path;
    json manifest;
};

// Callers that finalize MUST write back to .path, never a rebuilt one: the agent's manifest uses
// the recording stem, so a rebuilt path lands the terminal state in a file the uploader never ships.
inline Located locate(const std::string& recordingsRoot, const std::string& sessionId) {
    std::string path = sessionManifestPath(recordingsRoot, sessionId);
    json j = load(path);
    if (!j.is_object()) {
        path = manifestPathInDir(std::filesystem::path(recordingsRoot) / sessionId);
        j = path.empty() ? json{} : load(path);
    }
    if (j.is_object()) {
        return {path, j};
    }

    path = errorSessionManifestPath(recordingsRoot, sessionId);
    j = load(path);
    if (!j.is_object()) {
        path = manifestPathInDir(std::filesystem::path(errorDirPath(recordingsRoot)) / sessionId);
        j = path.empty() ? json{} : load(path);
    }
    if (j.is_object()) {
        return {path, j};
    }

    path = archivedJournalPath(recordingsRoot, sessionId);
    j = load(path);
    if (j.is_object()) {
        return {path, j};
    }
    return {};
}

inline json find(const std::string& recordingsRoot, const std::string& sessionId) {
    return locate(recordingsRoot, sessionId).manifest;
}

inline std::string findPath(const std::string& recordingsRoot, const std::string& sessionId) {
    return locate(recordingsRoot, sessionId).path;
}

inline std::string lastStatusPath(const std::string& recordingsRoot) {
    return (std::filesystem::path(recordingsRoot) / "journal" / "last_status.json").string();
}

// Surfaced on /health: identifier-free by design. Newer-wins so recovery can't shadow a clean stop.
inline bool recordLastStatus(const std::string& recordingsRoot, const std::string& state,
    const std::string& reason, int64_t endedAtNs) {
    const std::string path = lastStatusPath(recordingsRoot);
    const json existing = load(path);
    if (existing.is_object() && existing.value("endedAt", int64_t{0}) > endedAtNs) {
        return false;
    }
    json j;
    j["state"] = state;
    j["reason"] = reason;
    j["endedAt"] = endedAtNs;
    return atomicWrite(path, j);
}

inline json loadLastStatus(const std::string& recordingsRoot) {
    return load(lastStatusPath(recordingsRoot));
}

}
