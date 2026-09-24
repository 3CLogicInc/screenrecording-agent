#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ipc/client_connections.hpp"
#include "ipc/ipc_message.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

// Socket loopback tests are POSIX-only (Windows uses named pipes); the codec
// and ClientConnections tests run on both platforms.
#ifndef _WIN32
#include "ipc/unix_socket.hpp"
#include <unistd.h>
#endif

using namespace screenrec::ipc;

namespace {

// Spin until `pred` is true or the deadline passes (keeps the threaded tests
// from flaking on timing without sleeping a fixed large amount).
template <typename Pred>
bool waitUntil(Pred pred, int maxMs = 2000) {
    for (int i = 0; i < maxMs / 10 && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

#ifndef _WIN32
std::string tempSocketPath(const char* tag) {
    return std::string("/tmp/sr_test_") + tag + "_" + std::to_string(::getpid()) + ".sock";
}

void sendReady(UnixSocketClient& client) {
    IPCMessage ready;
    ready.type = MessageType::RESPONSE;
    ready.action = "ready";
    client.sendMessage(ready);
}

struct AgentSlot {
    std::atomic<ClientId> client{INVALID_CLIENT};

    explicit AgentSlot(IIpcServer& server) {
        server.setAgentResolver([this] { return client.load(); });
        server.setClientDisconnectHandler([this](const ClientId gone) {
            ClientId expected = gone;
            client.compare_exchange_strong(expected, INVALID_CLIENT);
        });
    }
};
#endif

}

TEST_CASE("IPCMessage round-trips through serialize/deserialize") {
    IPCMessage m;
    m.type = MessageType::COMMAND;
    m.action = "start_recording";
    m.sessionId = "session_abc123";
    m.payload = R"({"k":"v","n":42})";
    m.timestamp = 123456789;
    m.requestId = "req-42";

    const IPCMessage r = deserializeMessage(serializeMessage(m));
    CHECK(r.type == m.type);
    CHECK(r.action == m.action);
    CHECK(r.sessionId == m.sessionId);
    CHECK(r.payload == m.payload);
    CHECK(r.timestamp == m.timestamp);
    CHECK(r.requestId == m.requestId);
}

TEST_CASE("deserialize tolerates missing optional fields") {
    const IPCMessage r = deserializeMessage(R"({"type":0,"action":"ping"})");
    CHECK(r.action == "ping");
    CHECK(r.sessionId.empty());
    CHECK(r.payload.empty());
    CHECK(r.timestamp == 0);
}

TEST_CASE("malformed JSON becomes an ERROR_MSG parse_error") {
    const IPCMessage r = deserializeMessage("not json {");
    CHECK(r.type == MessageType::ERROR_MSG);
    CHECK(r.action == "parse_error");
}

TEST_CASE("payload with embedded quotes/newlines/backslashes survives") {
    IPCMessage m;
    m.type = MessageType::RESPONSE;
    m.action = "x";
    m.payload = "line1\nline2 \"quoted\" \\slash";
    const IPCMessage r = deserializeMessage(serializeMessage(m));
    CHECK(r.payload == m.payload);
}

TEST_CASE("ClientConnections assigns increasing ids, looks up, erases") {
    ClientConnections<int> c;
    std::atomic<int> ran{0};
    auto body = [&](ClientId) {
        ran++;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };

    const ClientId a = c.add(100, body);
    const ClientId b = c.add(200, body);
    CHECK(b > a);
    CHECK(c.lookup(a) == std::optional<int>(100));
    CHECK(c.lookup(b) == std::optional<int>(200));
    CHECK_FALSE(c.lookup(99999).has_value());

    c.erase(a);
    CHECK_FALSE(c.lookup(a).has_value());
    CHECK(c.lookup(b).has_value());

    c.joinAll();
    CHECK(ran.load() == 2);
}

TEST_CASE("reapFinished joins returned threads without hanging") {
    ClientConnections<int> c;
    c.add(1, [](ClientId) {});                 // returns immediately
    CHECK(waitUntil([] { return true; }, 50)); // let it finish
    c.reapFinished();                          // must not hang or crash
    c.joinAll();
    CHECK(true);
}

#ifndef _WIN32

TEST_CASE("client ready reaches server handler; server command reaches client") {
    const std::string path = tempSocketPath("hs");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    AgentSlot agent(server);
    std::atomic<bool> gotReady{false};
    server.setMessageHandler([&](const IPCMessage& m, ClientId client) {
        if (m.action == "ready") {
            agent.client = client;
            gotReady = true;
        }
    });
    REQUIRE(server.start());

    UnixSocketClient client(path);
    std::atomic<bool> gotCmd{false};
    IPCMessage received;
    client.setMessageHandler([&](const IPCMessage& m) {
        received = m;
        gotCmd = true;
    });
    REQUIRE(client.connect(2000));

    sendReady(client);
    CHECK(waitUntil([&] { return gotReady.load(); }));
    CHECK(server.hasConnectedAgent());

    IPCMessage cmd;
    cmd.type = MessageType::COMMAND;
    cmd.action = "configure";
    CHECK(server.sendToAgent(cmd));
    CHECK(waitUntil([&] { return gotCmd.load(); }));
    CHECK(received.action == "configure");

    client.disconnect();
    server.stop();
    ::unlink(path.c_str());
}

TEST_CASE("sendToAgentAndWait returns the agent's response") {
    const std::string path = tempSocketPath("rr");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    AgentSlot agent(server);
    server.setMessageHandler([&](const IPCMessage& m, ClientId client) {
        if (m.action == "ready") agent.client = client;
    });
    REQUIRE(server.start());

    UnixSocketClient client(path);
    client.setMessageHandler([&](const IPCMessage& m) {
        if (m.type == MessageType::COMMAND) {
            IPCMessage resp;
            resp.type = MessageType::RESPONSE;
            resp.action = m.action + "_done";
            resp.sessionId = m.sessionId;
            resp.requestId = m.requestId;
            client.sendMessage(resp);
        }
    });
    REQUIRE(client.connect(2000));
    sendReady(client);
    REQUIRE(waitUntil([&] { return server.hasConnectedAgent(); }));

    IPCMessage cmd;
    cmd.type = MessageType::COMMAND;
    cmd.action = "stop_recording";
    cmd.sessionId = "session_1";
    IPCMessage resp;
    CHECK(server.sendToAgentAndWait(cmd, resp, 2000));
    CHECK(resp.action == "stop_recording_done");
    CHECK(resp.sessionId == "session_1");

    client.disconnect();
    server.stop();
    ::unlink(path.c_str());
}

TEST_CASE("sendToAgentAndWait fails fast when the agent disconnects mid-wait") {
    const std::string path = tempSocketPath("dc");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    AgentSlot agent(server);
    server.setMessageHandler([&](const IPCMessage& m, ClientId client) {
        if (m.action == "ready") agent.client = client;
        // deliberately never respond to commands
    });
    REQUIRE(server.start());

    UnixSocketClient client(path);
    client.setMessageHandler([](const IPCMessage&) {});
    REQUIRE(client.connect(2000));
    sendReady(client);
    REQUIRE(waitUntil([&] { return server.hasConnectedAgent(); }));

    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        client.disconnect();
    });

    IPCMessage cmd;
    cmd.type = MessageType::COMMAND;
    cmd.action = "stop_recording";
    IPCMessage resp;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = server.sendToAgentAndWait(cmd, resp, 5000);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    killer.join();

    CHECK_FALSE(ok);                                  // returned because the agent dropped, not because of a reply
    CHECK(elapsed < std::chrono::milliseconds(3000)); // fast — not the full 5s timeout

    server.stop();
    ::unlink(path.c_str());
}

TEST_CASE("spontaneous RESPONSE (no requestId) cannot satisfy sendToAgentAndWait") {
    const std::string path = tempSocketPath("sp");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    AgentSlot agent(server);
    server.setMessageHandler([&](const IPCMessage& m, ClientId client) {
        if (m.action == "ready") agent.client = client;
    });
    REQUIRE(server.start());

    UnixSocketClient client(path);
    client.setMessageHandler([&](const IPCMessage& m) {
        if (m.type == MessageType::COMMAND) {
            // reply without the correlation id, like a stray "ready"
            IPCMessage stray;
            stray.type = MessageType::RESPONSE;
            stray.action = "ready";
            client.sendMessage(stray);
        }
    });
    REQUIRE(client.connect(2000));
    sendReady(client);
    REQUIRE(waitUntil([&] { return server.hasConnectedAgent(); }));

    IPCMessage cmd;
    cmd.type = MessageType::COMMAND;
    cmd.action = "get_status";
    IPCMessage resp;
    CHECK_FALSE(server.sendToAgentAndWait(cmd, resp, 500)); // must time out, not accept the stray

    client.disconnect();
    server.stop();
    ::unlink(path.c_str());
}

TEST_CASE("sendToAgentAndWait returns false when no agent is connected") {
    const std::string path = tempSocketPath("na");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    REQUIRE(server.start());
    CHECK_FALSE(server.hasConnectedAgent());

    IPCMessage cmd;
    cmd.action = "x";
    IPCMessage resp;
    CHECK_FALSE(server.sendToAgentAndWait(cmd, resp, 100));

    server.stop();
    ::unlink(path.c_str());
}

TEST_CASE("large payload under the 1 MiB frame cap round-trips over the socket") {
    const std::string path = tempSocketPath("big");
    ::unlink(path.c_str());

    UnixSocketServer server(path);
    AgentSlot agent(server);
    std::atomic<size_t> rxLen{0};
    server.setMessageHandler([&](const IPCMessage& m, ClientId c) {
        if (m.action == "ready") {
            agent.client = c;
        } else {
            rxLen = m.payload.size();
        }
    });
    REQUIRE(server.start());

    UnixSocketClient client(path);
    client.setMessageHandler([](const IPCMessage&) {});
    REQUIRE(client.connect(2000));
    sendReady(client);
    REQUIRE(waitUntil([&] { return server.hasConnectedAgent(); }));

    IPCMessage big;
    big.type = MessageType::COMMAND;
    big.action = "blob";
    big.payload = std::string(200000, 'x'); // 200 KB, well over one TCP segment
    CHECK(client.sendMessage(big));
    CHECK(waitUntil([&] { return rxLen.load() == 200000; }));

    client.disconnect();
    server.stop();
    ::unlink(path.c_str());
}

#endif // !_WIN32
