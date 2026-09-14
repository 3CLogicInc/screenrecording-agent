#pragma once

#include <string>

#include "storage/session_journal.hpp"

// Maps libobs stop codes to journal endReason values; OBS_OUTPUT_* is redefined here to build without libobs.
namespace screenrec::stop_reason {

inline constexpr int CODE_SUCCESS = 0;
inline constexpr int CODE_BAD_PATH = -1;
inline constexpr int CODE_CONNECT_FAILED = -2;
inline constexpr int CODE_INVALID_STREAM = -3;
inline constexpr int CODE_ERROR = -4;
inline constexpr int CODE_DISCONNECTED = -5;
inline constexpr int CODE_UNSUPPORTED = -6;
inline constexpr int CODE_NO_SPACE = -7;
inline constexpr int CODE_ENCODE_ERROR = -8;

struct Classification {
    const char* endReason;
    const char* errorCode; // machine-readable, surfaced as lastError.code
    std::string message;
};

[[nodiscard]] inline Classification classify(int obsStopCode) {
    switch (obsStopCode) {
    case CODE_SUCCESS:
        return {journal::END_CLIENT_STOP, "", ""};
    case CODE_NO_SPACE:
        return {journal::END_DISK_FULL, "obs_no_space",
            "Output stopped: no disk space left"};
    case CODE_ENCODE_ERROR:
        return {journal::END_ERROR, "obs_encode_error",
            "Output stopped: encoder failure"};
    case CODE_BAD_PATH:
        return {journal::END_ERROR, "obs_bad_path",
            "Output stopped: invalid output path"};
    case CODE_UNSUPPORTED:
        return {journal::END_ERROR, "obs_unsupported",
            "Output stopped: unsupported output settings"};
    case CODE_ERROR:
        return {journal::END_ERROR, "obs_error",
            "Output stopped: write/output error"};
    case CODE_CONNECT_FAILED:
    case CODE_INVALID_STREAM:
    case CODE_DISCONNECTED:
    default:
        return {journal::END_ERROR, "obs_stopped",
            "Output stopped unexpectedly (code " + std::to_string(obsStopCode) + ")"};
    }
}

}
