#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <fstream>
#include <sstream>
#include <string>

#include "core/configuration_manager.hpp"
#include "types.hpp"

using namespace screenrec;

namespace {

// Field-by-field so a failure names the exact field that diverged.
void checkVideoEqualsDefaults(const VideoConfig& v, const VideoConfig& d) {
    CHECK(v.width == d.width);
    CHECK(v.height == d.height);
    CHECK(v.fps == d.fps);
    CHECK(v.bitrate == d.bitrate);
    CHECK(v.maxBitrate == d.maxBitrate);
    CHECK(v.codec == d.codec);
    CHECK(v.preset == d.preset);
    CHECK(v.rateControl == d.rateControl);
    CHECK(v.crf == d.crf);
    CHECK(v.keyintSec == d.keyintSec);
    CHECK(v.encoder == d.encoder);
    CHECK(v.layout == d.layout);
}

void checkAudioEqualsDefaults(const AudioConfig& a, const AudioConfig& d) {
    CHECK(a.enabled == d.enabled);
    CHECK(a.sampleRate == d.sampleRate);
    CHECK(a.channels == d.channels);
    CHECK(a.bitrate == d.bitrate);
    CHECK(a.noiseSuppression == d.noiseSuppression);
}

}

TEST_CASE("compiled defaults pin the tuned recording profile") {
    // Mirrors config/config.json: a retune must change both sides and this pin.
    // PRD preset "Low" (720p @ 5fps); hevc pending browser-playback vs size call.
    const VideoConfig v;
    CHECK(v.width == 1920);
    CHECK(v.height == 1080);
    CHECK(v.fps == 5);
    CHECK(v.bitrate == 500000);
    CHECK(v.codec == "h264");
    CHECK(v.preset == "slower");
    CHECK(v.rateControl == "CRF");
    CHECK(v.crf == 27);
    CHECK(v.keyintSec == 30);
    CHECK(v.encoder == "auto");
    CHECK(v.layout == "equalTiles");

    const AudioConfig a;
    CHECK(a.enabled == true);
    CHECK(a.sampleRate == 44100);
    CHECK(a.channels == 1);
    CHECK(a.bitrate == 32000);
    CHECK(a.noiseSuppression == "none");
}

TEST_CASE("empty JSON yields the compiled defaults (absent recording block)") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson("{}"));
    const auto cfg = mgr.getConfiguration();
    checkVideoEqualsDefaults(cfg.recording.video, VideoConfig{});
    checkAudioEqualsDefaults(cfg.recording.audio, AudioConfig{});
}

TEST_CASE("empty video/audio blocks fall back per-field to the compiled defaults") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"recording":{"video":{},"audio":{}}})"));
    const auto cfg = mgr.getConfiguration();
    checkVideoEqualsDefaults(cfg.recording.video, VideoConfig{});
    checkAudioEqualsDefaults(cfg.recording.audio, AudioConfig{});
}

TEST_CASE("partial config keeps explicit values and defaults the rest") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(
        R"({"recording":{"video":{"codec":"hevc","crf":23},"audio":{"channels":2}}})"));
    const auto cfg = mgr.getConfiguration();

    CHECK(cfg.recording.video.codec == "hevc");
    CHECK(cfg.recording.video.crf == 23);
    CHECK(cfg.recording.video.keyintSec == VideoConfig{}.keyintSec);
    CHECK(cfg.recording.video.preset == VideoConfig{}.preset);
    CHECK(cfg.recording.video.fps == VideoConfig{}.fps);

    CHECK(cfg.recording.audio.channels == 2);
    CHECK(cfg.recording.audio.bitrate == AudioConfig{}.bitrate);
    CHECK(cfg.recording.audio.enabled == AudioConfig{}.enabled);
}

TEST_CASE("out-of-range numeric fields are clamped to the API bounds with warnings") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"recording":{
        "video":{"fps":300,"crf":200,"keyint_sec":0,"width":100000,"bitrate":90000000},
        "audio":{"channels":9,"bitrate":0}}})"));
    const auto cfg = mgr.getConfiguration();

    CHECK(cfg.recording.video.fps == 120);
    CHECK(cfg.recording.video.crf == 51);
    CHECK(cfg.recording.video.keyintSec == 1);
    CHECK(cfg.recording.video.width == 7680);
    CHECK(cfg.recording.video.bitrate == 50000000);
    CHECK(cfg.recording.audio.channels == 8);
    CHECK(cfg.recording.audio.bitrate == 1);

    const auto warnings = mgr.loadWarnings();
    CHECK(warnings.size() == 7);
    // Each warning names the offending field so the log line is actionable.
    for (const char* field : {"video.fps", "video.crf", "video.keyint_sec",
             "video.width", "video.bitrate",
             "audio.channels", "audio.bitrate"}) {
        bool found = false;
        for (const auto& w : warnings) {
            if (w.find(field) != std::string::npos) {
                found = true;
                break;
            }
        }
        CHECK_MESSAGE(found, "missing warning for " << field);
    }
}

TEST_CASE("in-range values load untouched and produce no warnings") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"recording":{
        "video":{"fps":120,"crf":0,"keyint_sec":60,"max_bitrate":0},
        "audio":{"channels":8,"bitrate":512000}}})"));
    const auto cfg = mgr.getConfiguration();

    CHECK(cfg.recording.video.fps == 120);
    CHECK(cfg.recording.video.crf == 0);
    CHECK(cfg.recording.video.keyintSec == 60);
    CHECK(cfg.recording.video.maxBitrate == 0); // 0 = auto stays legal
    CHECK(cfg.recording.audio.channels == 8);
    CHECK(cfg.recording.audio.bitrate == 512000);
    CHECK(mgr.loadWarnings().empty());
}

TEST_CASE("upload block parses transport tuning and defaults the rest") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"upload":{
        "uploadThreads":4,"connectTimeoutSeconds":10,
        "lowSpeedLimitBytesPerSec":500,"lowSpeedTimeSeconds":30,
        "maxUploadSpeedBytesPerSec":1048576}})"));
    const auto cfg = mgr.getConfiguration();

    CHECK(cfg.upload.uploadThreads == 4);
    CHECK(cfg.upload.connectTimeoutSeconds == 10);
    CHECK(cfg.upload.lowSpeedLimitBytesPerSec == 500);
    CHECK(cfg.upload.lowSpeedTimeSeconds == 30);
    CHECK(cfg.upload.maxUploadSpeedBytesPerSec == 1048576);
    CHECK(cfg.upload.attemptsPerRound == UploadConfig{}.attemptsPerRound);
    CHECK(cfg.upload.retryDelaySeconds == UploadConfig{}.retryDelaySeconds);
    CHECK(cfg.upload.pendingRescanSeconds == UploadConfig{}.pendingRescanSeconds);
    CHECK(mgr.loadWarnings().empty());
}

TEST_CASE("upload speed cap defaults to 4 Mbps and 0 stays legal as unlimited") {
    CHECK(UploadConfig{}.maxUploadSpeedBytesPerSec == 512 * 1024);

    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"upload":{"maxUploadSpeedBytesPerSec":0}})"));
    CHECK(mgr.getConfiguration().upload.maxUploadSpeedBytesPerSec == 0);
    CHECK(mgr.loadWarnings().empty());
}

TEST_CASE("log rotation knobs parse from MB and clamp out-of-range values") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"logging":{"maxFileSizeMB":20,"maxFiles":8}})"));
    CHECK(mgr.getConfiguration().logging.maxFileSizeBytes == 20UL * 1024 * 1024);
    CHECK(mgr.getConfiguration().logging.maxFiles == 8);
    CHECK(mgr.loadWarnings().empty());

    REQUIRE(mgr.loadFromJson(R"({"logging":{"maxFileSizeMB":0,"maxFiles":5000}})"));
    CHECK(mgr.getConfiguration().logging.maxFileSizeBytes == 1UL * 1024 * 1024);
    CHECK(mgr.getConfiguration().logging.maxFiles == 100);
    CHECK(mgr.loadWarnings().size() == 2);

    REQUIRE(mgr.loadFromJson(R"({"logging":{}})"));
    CHECK(mgr.getConfiguration().logging.maxFileSizeBytes == LoggingConfig{}.maxFileSizeBytes);
    CHECK(mgr.getConfiguration().logging.maxFiles == LoggingConfig{}.maxFiles);
}

TEST_CASE("uploadUrlsEndpoint parses from the top level and round-trips through save") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"upload":{}})"));
    CHECK(mgr.getConfiguration().upload.urlsEndpoint.empty());

    const std::string endpoint =
        "https://host/7.6.0/api/v1/840/cti/agent/screen-recording/{referenceId}/upload-urls";
    REQUIRE(mgr.loadFromJson(R"({"uploadUrlsEndpoint":")" + endpoint + R"("})"));
    CHECK(mgr.getConfiguration().upload.urlsEndpoint == endpoint);

    ConfigurationManager roundTrip;
    REQUIRE(roundTrip.loadFromJson(mgr.saveToJson()));
    CHECK(roundTrip.getConfiguration().upload.urlsEndpoint == endpoint);
}

TEST_CASE("a non-string or absent uploadUrlsEndpoint leaves the stored one untouched") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"uploadUrlsEndpoint":"https://host/{referenceId}/upload-urls"})"));
    REQUIRE(mgr.loadFromJson(R"({"uploadUrlsEndpoint":42})"));
    CHECK(mgr.getConfiguration().upload.urlsEndpoint == "https://host/{referenceId}/upload-urls");

    REQUIRE(mgr.loadFromJson(R"({"upload":{"uploadThreads":2}})"));
    CHECK(mgr.getConfiguration().upload.urlsEndpoint == "https://host/{referenceId}/upload-urls");
}

TEST_CASE("JWT + loki credential fields parse with shipped defaults") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"server":{},"logging":{}})"));
    CHECK(mgr.getConfiguration().server.jwtPublicKeyPath == "certs/public.pem");
    CHECK(mgr.getConfiguration().server.jwtPublicKeysByKit.empty());
    CHECK(mgr.getConfiguration().server.jwtIssuer == "3clogic");
    CHECK(mgr.getConfiguration().logging.credentialsEndpoint.empty());

    REQUIRE(mgr.loadFromJson(R"({
        "server":{"jwtPublicKeyPath":"certs/other.pem","jwtIssuer":"acme",
                  "jwtPublicKeysByKit":{"3clogic-qa":"certs/public-qa.pem","bad":""}},
        "logging":{"credentialsEndpoint":"https://api.example/loki/creds"}})"));
    CHECK(mgr.getConfiguration().server.jwtPublicKeyPath == "certs/other.pem");
    CHECK(mgr.getConfiguration().server.jwtIssuer == "acme");
    REQUIRE(mgr.getConfiguration().server.jwtPublicKeysByKit.size() == 1);
    CHECK(mgr.getConfiguration().server.jwtPublicKeysByKit.at("3clogic-qa") ==
          "certs/public-qa.pem");
    CHECK(mgr.getConfiguration().logging.credentialsEndpoint == "https://api.example/loki/creds");
}

TEST_CASE("loki credentials load into memory but never serialize back out") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(
        R"({"logUploadCred":{"endpoint":"https://l/push","apiKey":"ak-secret","secretKey":"sk-secret"}})"));
    CHECK(mgr.getConfiguration().logging.lokiApiKey == "ak-secret");
    CHECK(mgr.getConfiguration().logging.lokiEndpoint == "https://l/push");

    const std::string out = mgr.saveToJson();
    CHECK(out.find("logUploadCred") == std::string::npos);
    CHECK(out.find("ak-secret") == std::string::npos);
    CHECK(out.find("sk-secret") == std::string::npos);
    CHECK(out.find("https://l/push") == std::string::npos);
}

TEST_CASE("server.ports is the only port source; missing or empty falls back to default") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"server":{"ports":[9005,9006]}})"));
    CHECK(mgr.getConfiguration().server.ports == std::vector<uint16_t>{9005, 9006});

    REQUIRE(mgr.loadFromJson(R"({"server":{}})"));
    CHECK(mgr.getConfiguration().server.ports == ServerConfig{}.ports);

    REQUIRE(mgr.loadFromJson(R"({"server":{"ports":[0, 70000]}})"));
    CHECK(mgr.getConfiguration().server.ports == ServerConfig{}.ports);
}

TEST_CASE("uploadThreads=0 would hang every upload, so it clamps with a warning") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"upload":{"uploadThreads":0}})"));
    CHECK(mgr.getConfiguration().upload.uploadThreads == 1);
    CHECK(mgr.loadWarnings().size() == 1);

    REQUIRE(mgr.loadFromJson(R"({"upload":{"uploadThreads":64}})"));
    CHECK(mgr.getConfiguration().upload.uploadThreads == 16);
    const auto warnings = mgr.loadWarnings();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("upload.uploadThreads") != std::string::npos);
}

TEST_CASE("warnings from a previous load are cleared by the next load") {
    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(R"({"recording":{"video":{"crf":200}}})"));
    CHECK(mgr.loadWarnings().size() == 1);
    REQUIRE(mgr.loadFromJson("{}"));
    CHECK(mgr.loadWarnings().empty());
}

TEST_CASE("shipped config/config.json agrees with the compiled defaults") {
    std::ifstream file(SHIPPED_CONFIG_JSON);
    REQUIRE_MESSAGE(file.is_open(), "cannot open shipped config: " SHIPPED_CONFIG_JSON);
    std::stringstream buffer;
    buffer << file.rdbuf();

    ConfigurationManager mgr;
    REQUIRE(mgr.loadFromJson(buffer.str()));
    CHECK(mgr.loadWarnings().empty()); // shipped values must be in-bounds
    const auto cfg = mgr.getConfiguration();

    const VideoConfig vd;
    CHECK(cfg.recording.video.width == vd.width);
    CHECK(cfg.recording.video.height == vd.height);
    CHECK(cfg.recording.video.fps == vd.fps);
    CHECK(cfg.recording.video.bitrate == vd.bitrate);
    // maxBitrate intentionally differs (file: explicit VBR ceiling, default:
    // 0 = auto sentinel); both ignored under the shipped CRF rate control.
    CHECK(cfg.recording.video.codec == vd.codec);
    CHECK(cfg.recording.video.preset == vd.preset);
    CHECK(cfg.recording.video.rateControl == vd.rateControl);
    CHECK(cfg.recording.video.crf == vd.crf);
    CHECK(cfg.recording.video.keyintSec == vd.keyintSec);
    CHECK(cfg.recording.video.encoder == vd.encoder);
    CHECK(cfg.recording.video.layout == vd.layout);

    checkAudioEqualsDefaults(cfg.recording.audio, AudioConfig{});
}
