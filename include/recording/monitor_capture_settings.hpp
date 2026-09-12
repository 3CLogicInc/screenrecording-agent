#pragma once

#include <cstdint>
#include <string>

#include "types.hpp"

namespace screenrec::capture {

inline constexpr int64_t MONITOR_CAPTURE_METHOD_AUTO = 0;
inline constexpr int64_t MONITOR_CAPTURE_METHOD_DXGI = 1;
inline constexpr int64_t MONITOR_CAPTURE_METHOD_WGC = 2;

struct MonitorCaptureSettings {
    std::string monitorId;
    bool captureCursor;
    int64_t method;
};

[[nodiscard]] inline MonitorCaptureSettings monitorCaptureSettings(const MonitorInfo& monitor) {
    return {monitor.name, true, MONITOR_CAPTURE_METHOD_AUTO};
}

}
