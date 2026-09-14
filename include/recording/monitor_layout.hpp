#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "types.hpp"

namespace screenrec {

// Axis-aligned bounding box of the virtual desktop, in canvas pixels.
struct MonitorBounds {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
};

[[nodiscard]] inline int layoutWidth(const MonitorInfo& m) {
    return static_cast<int>(std::lround(static_cast<double>(m.width) * m.renderScale));
}
[[nodiscard]] inline int layoutHeight(const MonitorInfo& m) {
    return static_cast<int>(std::lround(static_cast<double>(m.height) * m.renderScale));
}

[[nodiscard]] inline bool getCompositeBounds(const std::vector<MonitorInfo>& monitors,
    MonitorBounds& bounds) {
    if (monitors.empty()) {
        return false;
    }
    bounds.minX = monitors[0].x;
    bounds.minY = monitors[0].y;
    bounds.maxX = monitors[0].x + layoutWidth(monitors[0]);
    bounds.maxY = monitors[0].y + layoutHeight(monitors[0]);
    for (const auto& monitor : monitors) {
        bounds.minX = std::min(bounds.minX, monitor.x);
        bounds.minY = std::min(bounds.minY, monitor.y);
        bounds.maxX = std::max(bounds.maxX, monitor.x + layoutWidth(monitor));
        bounds.maxY = std::max(bounds.maxY, monitor.y + layoutHeight(monitor));
    }
    return true;
}

// macOS mixed-DPI: origins converted with ONE canonical scale (per-display scaling
// overlaps/gaps the tiling); renderScale enlarges lower-DPI displays. No-op on Windows.
inline void normalizeMixedDpiLayout(std::vector<MonitorInfo>& monitors) {
    float maxScale = 0.0f;
    for (const auto& monitor : monitors) {
        maxScale = std::max(maxScale, monitor.dpiScaleX);
    }
    if (maxScale <= 0.0f) {
        maxScale = 1.0f;
    }
    for (auto& monitor : monitors) {
        monitor.x = static_cast<int>(std::lround(static_cast<double>(monitor.x) * maxScale));
        monitor.y = static_cast<int>(std::lround(static_cast<double>(monitor.y) * maxScale));
        monitor.renderScale = monitor.dpiScaleX > 0.0f ? maxScale / monitor.dpiScaleX : 1.0f;
    }
}

inline void normalizeEqualTiles(std::vector<MonitorInfo>& monitors) {
    int tileHeight = 0;
    for (const auto& monitor : monitors) {
        tileHeight = std::max(tileHeight, monitor.height);
    }
    if (tileHeight <= 0) {
        return;
    }
    std::vector<std::size_t> order(monitors.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return std::make_pair(monitors[a].x, monitors[a].y) < std::make_pair(monitors[b].x, monitors[b].y);
    });
    int cursorX = 0;
    for (const std::size_t i : order) {
        auto& monitor = monitors[i];
        monitor.renderScale = monitor.height > 0
                                  ? static_cast<float>(tileHeight) / static_cast<float>(monitor.height)
                                  : 1.0f;
        monitor.x = cursorX;
        monitor.y = 0;
        cursorX += layoutWidth(monitor);
    }
}

// Fit a composite into a FIXED canvas (cannot change while encoding): uniform
// centered scale, may exceed 1 so a post-unplug composite expands to fill the canvas.
struct CanvasFit {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

[[nodiscard]] inline CanvasFit fitCompositeToCanvas(uint32_t canvasW, uint32_t canvasH,
    int compositeW, int compositeH) {
    if (canvasW == 0 || canvasH == 0 || compositeW <= 0 || compositeH <= 0) {
        return {};
    }
    const float scale = std::min(static_cast<float>(canvasW) / static_cast<float>(compositeW),
        static_cast<float>(canvasH) / static_cast<float>(compositeH));
    return {scale,
        (static_cast<float>(canvasW) - static_cast<float>(compositeW) * scale) / 2.0f,
        (static_cast<float>(canvasH) - static_cast<float>(compositeH) * scale) / 2.0f};
}

// PRD cap: record the main monitor plus up to two extended screens.
inline constexpr std::size_t MAX_RECORDED_MONITORS = 3;

// Primary plus first extended monitors in detection order, up to maxCount (0 = no cap):
// deterministic per PRD "main + same two monitors" every session.
[[nodiscard]] inline std::vector<MonitorInfo> selectRecordedMonitors(
    const std::vector<MonitorInfo>& monitors, std::size_t maxCount = MAX_RECORDED_MONITORS) {
    if (maxCount == 0 || monitors.size() <= maxCount) {
        return monitors;
    }

    const auto primary = std::find_if(monitors.begin(), monitors.end(),
        [](const MonitorInfo& m) { return m.isPrimary; });
    std::size_t extendedBudget = maxCount - (primary != monitors.end() ? 1 : 0);

    std::vector<MonitorInfo> selected;
    selected.reserve(maxCount);
    for (auto it = monitors.begin(); it != monitors.end(); ++it) {
        if (it == primary) {
            selected.push_back(*it);
        } else if (extendedBudget > 0) {
            selected.push_back(*it);
            --extendedBudget;
        }
    }
    return selected;
}

}
