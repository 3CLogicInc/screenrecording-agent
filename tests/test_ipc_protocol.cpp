#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <json.hpp>

#include "ipc/protocol.hpp"

using nlohmann::json;
using screenrec::ipc::protocol::PROTOCOL_VERSION;
using screenrec::ipc::protocol::ReadyPayload;
namespace protocol = screenrec::ipc::protocol;

TEST_CASE("ready payload round-trips through JSON") {
    ReadyPayload p;
    p.protocolVersion = PROTOCOL_VERSION;
    p.agentVersion = "1.0.0";
    p.platform = "macos";
    p.monitorCount = 2;
    p.screenRecordingPermission = 1;

    const auto parsed = ReadyPayload::fromJson(p.toJson());
    CHECK(parsed.protocolVersion == PROTOCOL_VERSION);
    CHECK(parsed.agentVersion == "1.0.0");
    CHECK(parsed.platform == "macos");
    CHECK(parsed.monitorCount == 2);
    CHECK(parsed.screenRecordingPermission == 1);
}

TEST_CASE("a pre-versioning agent payload parses with v1 defaults") {
    // What agents sent before protocol.hpp existed: no version, no monitors.
    const json legacy = {
        {"agentVersion", "0.9.0"},
        {"platform", "windows"},
    };
    const auto p = ReadyPayload::fromJson(legacy);
    CHECK(p.protocolVersion == 1);
    CHECK(p.agentVersion == "0.9.0");
    CHECK(p.monitorCount == -1);
    CHECK(p.screenRecordingPermission == -1);
}

TEST_CASE("unknown fields from a NEWER agent are ignored, known ones read") {
    json future = ReadyPayload{}.toJson();
    future["protocolVersion"] = 2;
    future["someNewCapability"] = true;
    future["monitorCount"] = 3;

    const auto p = ReadyPayload::fromJson(future);
    CHECK(p.protocolVersion == 2);
    CHECK(p.monitorCount == 3);
}

TEST_CASE("malformed field types keep defaults instead of throwing") {
    const json bad = {
        {"screenRecordingPermission", "yes"}, // wrong type: string, not bool
        {"agentVersion", "1.2.3"},
    };
    const auto p = ReadyPayload::fromJson(bad);
    CHECK(p.screenRecordingPermission == -1);
    CHECK(p.agentVersion == "1.2.3");
}

TEST_CASE("permission and monitor fields are omitted from JSON when unknown") {
    const auto j = ReadyPayload{}.toJson();
    CHECK_FALSE(j.contains("monitorCount"));
    CHECK_FALSE(j.contains("screenRecordingPermission"));
    CHECK(j.contains("protocolVersion"));
}

TEST_CASE("action constants match the wire strings the fleet already uses") {
    // These strings are the deployed contract (N/N-1): changing one breaks
    // older peers. This test makes any change deliberate.
    CHECK(std::string(protocol::ACTION_READY) == "ready");
    CHECK(std::string(protocol::ACTION_REGISTERED) == "registered");
    CHECK(std::string(protocol::ACTION_START_RECORDING) == "start_recording");
    CHECK(std::string(protocol::ACTION_STOP_RECORDING) == "stop_recording");
    CHECK(std::string(protocol::ACTION_RECORDING_STARTED) == "recording_started");
    CHECK(std::string(protocol::ACTION_RECORDING_STOPPED) == "recording_stopped");
    CHECK(std::string(protocol::ACTION_SHUTDOWN) == "shutdown");
}
