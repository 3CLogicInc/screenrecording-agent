#pragma once

#include <algorithm>
#include <cstdint>

namespace screenrec {

struct OutputDimensions {
    uint32_t width;
    uint32_t height;
};

// H.264/HEVC 4:2:0 needs even dimensions; floor of 2 avoids zero-sized frames.
[[nodiscard]] constexpr uint32_t roundDownEven(uint32_t v) {
    return v < 2 ? 2 : v - (v % 2);
}

// Widest frame common H.264 decoder levels accept; binds only on 4+ monitor canvases.
inline constexpr uint32_t MAX_ENCODE_WIDTH = 4096;

// Width follows aspect, capped at MAX_ENCODE_WIDTH, never upscaled. Height is the cap (not a WxH box) so
// per-monitor legibility survives multi-monitor canvases; zero inputs fall back to the even-clamped canvas.
[[nodiscard]] constexpr OutputDimensions computeOutputDimensions(uint32_t canvasW, uint32_t canvasH,
    uint32_t targetHeight) {
    if (canvasW == 0 || canvasH == 0 || targetHeight == 0) {
        return {roundDownEven(canvasW), roundDownEven(canvasH)};
    }
    const double scale = std::min({static_cast<double>(targetHeight) / canvasH,
        static_cast<double>(MAX_ENCODE_WIDTH) / canvasW,
        1.0});
    return {roundDownEven(static_cast<uint32_t>(canvasW * scale)),
        roundDownEven(static_cast<uint32_t>(canvasH * scale))};
}

}
