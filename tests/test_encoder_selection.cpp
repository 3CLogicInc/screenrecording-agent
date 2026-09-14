#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <unordered_set>

#include "recording/encoder_selection.hpp"

using namespace screenrec;
using namespace screenrec::encoder;

namespace {

VideoConfig config(const std::string& codec = "h264", const std::string& enc = "",
    const std::string& rateControl = "CRF") {
    VideoConfig v;
    v.codec = codec;
    v.encoder = enc;
    v.rateControl = rateControl;
    return v;
}

}

TEST_CASE("explicit software request wins over available hardware") {
    const std::unordered_set<std::string> available = {"jim_nvenc", "obs_x264"};
    for (const auto& forced : {"x264", "CPU", "Software"}) {
        const auto choice = selectVideoEncoder(config("h264", forced), available);
        CHECK(choice.id == "obs_x264");
        CHECK_FALSE(choice.isHardware);
        CHECK(choice.warnings.empty());
    }
}

TEST_CASE("software + HEVC request degrades to x264 with a warning") {
    const auto choice = selectVideoEncoder(config("hevc", "x264"), {"jim_hevc_nvenc"});
    CHECK(choice.id == "obs_x264");
    REQUIRE(choice.warnings.size() == 1);
    CHECK(choice.warnings[0].find("no software HEVC") != std::string::npos);
}

TEST_CASE("HEVC picks the highest-priority available hardware encoder") {
    // Both a legacy and a newer NVENC HEVC registered: newest wins.
    auto choice = selectVideoEncoder(config("hevc"),
        {"ffmpeg_hevc_nvenc", "obs_nvenc_hevc_tex", "obs_x264"});
    CHECK(choice.id == "obs_nvenc_hevc_tex");
    CHECK(choice.isHardware);

    // Apple VT HEVC on a Mac-like build.
    choice = selectVideoEncoder(config("h265"),
        {"com.apple.videotoolbox.videoencoder.ave.hevc",
            "com.apple.videotoolbox.videoencoder.ave.avc", "obs_x264"});
    CHECK(choice.id == "com.apple.videotoolbox.videoencoder.ave.hevc");
}

TEST_CASE("HEVC with no HEVC encoder falls back to H.264 hardware with a warning") {
    const auto choice = selectVideoEncoder(config("hevc"), {"jim_nvenc", "obs_x264"});
    CHECK(choice.id == "jim_nvenc");
    CHECK(choice.isHardware);
    REQUIRE(choice.warnings.size() == 1);
    CHECK(choice.warnings[0].find("falling back to H.264") != std::string::npos);
}

TEST_CASE("H.264 vendor priority: NVIDIA > AMD > Intel > Apple") {
    CHECK(selectVideoEncoder(config(),
              {"h264_texture_amf", "jim_nvenc", "obs_qsv11_v2"})
              .id == "jim_nvenc");
    CHECK(selectVideoEncoder(config(),
              {"obs_qsv11_v2", "h264_texture_amf"})
              .id == "h264_texture_amf");
    CHECK(selectVideoEncoder(config(),
              {"com.apple.videotoolbox.videoencoder.ave.avc", "obs_qsv11"})
              .id == "obs_qsv11");
    CHECK(selectVideoEncoder(config(),
              {"com.apple.videotoolbox.videoencoder.ave.avc"})
              .id ==
          "com.apple.videotoolbox.videoencoder.ave.avc");
}

TEST_CASE("no hardware encoder at all falls back to x264") {
    const auto choice = selectVideoEncoder(config(), {"obs_x264", "ffmpeg_aac"});
    CHECK(choice.id == "obs_x264");
    CHECK_FALSE(choice.isHardware);
    CHECK(choice.warnings.empty());

    // Even an empty registry resolves (encoder creation will fail loudly later).
    CHECK(selectVideoEncoder(config(), {}).id == "obs_x264");
}

TEST_CASE("rate control resolves case-insensitively with CRF as the default") {
    CHECK(resolveRateControl(config("h264", "", "CBR")) == RateControl::CBR);
    CHECK(resolveRateControl(config("h264", "", "cbr")) == RateControl::CBR);
    CHECK(resolveRateControl(config("h264", "", "VBR")) == RateControl::VBR);
    CHECK(resolveRateControl(config("h264", "", "abr")) == RateControl::VBR);
    CHECK(resolveRateControl(config("h264", "", "CRF")) == RateControl::QUALITY);
    CHECK(resolveRateControl(config("h264", "", "anything")) == RateControl::QUALITY);
}

TEST_CASE("bitrate conversion clamps to at least 1 kbps and defaults max to 2x") {
    VideoConfig v = config();
    v.bitrate = 2500000;
    CHECK(bitrateKbps(v) == 2500);
    CHECK(maxBitrateKbps(v) == 5000);

    v.maxBitrate = 4000000;
    CHECK(maxBitrateKbps(v) == 4000);

    v.bitrate = 10; // sub-kbps config
    CHECK(bitrateKbps(v) == 1);

    v.keyintSec = 0;
    CHECK(keyintSec(v) == 1);
}

TEST_CASE("wantsHevc accepts codec spellings; isHevcEncoder matches ids") {
    CHECK(wantsHevc(config("hevc")));
    CHECK(wantsHevc(config("H265")));
    CHECK(wantsHevc(config("h.265")));
    CHECK_FALSE(wantsHevc(config("h264")));

    CHECK(isHevcEncoder("jim_hevc_nvenc"));
    CHECK(isHevcEncoder("obs_qsv11_hevc"));
    CHECK_FALSE(isHevcEncoder("jim_nvenc"));
}
