#pragma once

#include <algorithm>
#include <cstdint>
#include <array>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "types.hpp"

// Pure policy: no OBS calls — the engine supplies `available` and applies the choice.
namespace screenrec::encoder {

// CBR: hard size cap, pads static screens. VBR/ABR: capped VBR. Anything else: CRF, with UNBOUNDED size.
enum class RateControl : uint8_t { CBR,
    VBR,
    QUALITY };

[[nodiscard]] inline RateControl resolveRateControl(const VideoConfig& video) {
    std::string rc = video.rateControl;
    std::transform(rc.begin(), rc.end(), rc.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (rc == "CBR") {
        return RateControl::CBR;
    }
    if (rc == "VBR" || rc == "ABR") {
        return RateControl::VBR;
    }
    return RateControl::QUALITY;
}

// OBS encoders take kbps; config stores bits per second.
[[nodiscard]] inline int bitrateKbps(const VideoConfig& video) {
    return static_cast<int>(std::max(video.bitrate / 1000u, 1u));
}

// VBR ceiling: explicit max_bitrate if configured, otherwise 2x the target.
[[nodiscard]] inline int maxBitrateKbps(const VideoConfig& video) {
    if (video.maxBitrate > 0) {
        return static_cast<int>(std::max(video.maxBitrate / 1000u, 1u));
    }
    return bitrateKbps(video) * 2;
}

[[nodiscard]] inline int keyintSec(const VideoConfig& video) {
    return static_cast<int>(std::max(video.keyintSec, 1u));
}

// HEVC is ~30-50% smaller than H.264 at equal quality; no software HEVC is bundled, so it resolves to
// hardware or falls back to H.264.
[[nodiscard]] inline bool wantsHevc(const VideoConfig& video) {
    std::string c = video.codec;
    std::transform(c.begin(), c.end(), c.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return c == "hevc" || c == "h265" || c == "h.265";
}

[[nodiscard]] inline bool isHevcEncoder(const std::string& id) {
    return id.find("hevc") != std::string::npos || id.find("h265") != std::string::npos;
}

struct EncoderChoice {
    std::string id;
    bool isHardware = false;
    std::vector<std::string> info;
    std::vector<std::string> warnings;
};

// Explicit software request wins; then HEVC hardware (if requested); then GPU H.264 (NVIDIA > AMD > Intel > Apple, newest per vendor); x264 last.
[[nodiscard]] inline EncoderChoice selectVideoEncoder(
    const VideoConfig& video, const std::unordered_set<std::string>& available) {
    EncoderChoice choice;

    std::string enc = video.encoder;
    std::transform(enc.begin(), enc.end(), enc.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (enc == "x264" || enc == "cpu" || enc == "software") {
        if (wantsHevc(video)) {
            choice.warnings.emplace_back(
                "HEVC requested with software encoder, but no software HEVC is bundled; "
                "using x264 (H.264)");
        }
        choice.id = "obs_x264";
        choice.info.emplace_back("✓ Selected: software x264 (CPU encoding) - forced by config");
        return choice;
    }

    if (!enc.empty() && enc != "auto" && available.count(video.encoder) != 0) {
        choice.id = video.encoder;
        choice.isHardware = video.encoder != "obs_x264";
        choice.info.emplace_back("✓ Selected: " + video.encoder + " - exact id forced by config");
        return choice;
    }

    if (wantsHevc(video)) {
        // NVENC (newest first) > AMD AMF > Intel QSV (oneVPL > deprecated MSDK) > Apple VT; unknown ids are skipped.
        constexpr std::array<const char*, 8> hevcIds = {
            "obs_nvenc_hevc_tex",
            "jim_hevc_nvenc",
            "ffmpeg_hevc_nvenc",
            "h265_texture_amf",
            "obs_qsv11_v2_hevc",
            "obs_qsv11_hevc_v2",
            "obs_qsv11_hevc",
            "com.apple.videotoolbox.videoencoder.ave.hevc",
        };
        for (const char* id : hevcIds) {
            if (available.count(id) != 0) {
                choice.id = id;
                choice.isHardware = true;
                choice.info.emplace_back(std::string("✓ Selected HEVC hardware encoder: ") + id);
                return choice;
            }
        }
        choice.warnings.emplace_back(
            "HEVC requested but no HEVC encoder is available in this build; "
            "falling back to H.264");
    }

    struct GpuEncoder {
        const char* id;
        const char* label;
    };
    constexpr std::array<GpuEncoder, 8> gpuEncoders = {{
        {"obs_nvenc_h264_tex", "NVIDIA NVENC (OBS 30.2+)"},
        {"jim_nvenc", "NVIDIA NVENC"},
        {"ffmpeg_nvenc", "NVIDIA NVENC (legacy)"},
        {"h264_texture_amf", "AMD AMF"},
        {"amd_amf_h264", "AMD AMF (legacy)"},
        {"obs_qsv11_v2", "Intel QSV (oneVPL)"},
        {"obs_qsv11", "Intel QSV (deprecated MSDK)"},
        {"com.apple.videotoolbox.videoencoder.ave.avc", "Apple VideoToolbox"},
    }};
    for (const auto& e : gpuEncoders) {
        if (available.count(e.id) != 0) {
            choice.id = e.id;
            choice.isHardware = true;
            choice.info.emplace_back(std::string("✓ Selected: ") + e.label + " (" + e.id +
                                     ") - GPU Hardware Encoding");
            return choice;
        }
    }

    choice.id = "obs_x264";
    choice.info.emplace_back("ℹ No GPU encoder found, using software x264 (CPU encoding)");
    return choice;
}

}
