#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ipc/ipc_message.hpp"

namespace screenrec::ipc {

using ClientId = std::uint64_t;
inline constexpr ClientId INVALID_CLIENT = 0;

class IIpcServer {
public:
    using MessageHandler = std::function<void(const IPCMessage&, ClientId client)>;

    virtual ~IIpcServer() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;

    virtual void setMessageHandler(const MessageHandler& handler) = 0;
    virtual bool sendMessage(ClientId client, const IPCMessage& message) = 0;

    [[nodiscard]] virtual std::uint32_t clientProcessId(ClientId client) const {
        (void)client;
        return 0;
    }

    virtual bool waitForClientDisconnect(ClientId client, std::chrono::milliseconds timeout) = 0;
    virtual bool waitForAgentDisconnect(std::chrono::milliseconds timeout) = 0;

    [[nodiscard]] virtual bool hasConnectedAgent() const = 0;
    virtual void setAgentResolver(std::function<ClientId()> resolver) = 0;

    // Runs on the transport reader thread; the handler must not call back into sendToAgentAndWait().
    virtual void setClientDisconnectHandler(std::function<void(ClientId)> handler) = 0;

    virtual bool sendToAgent(const IPCMessage& message) = 0;
    virtual bool sendToAgentAndWait(const IPCMessage& message, IPCMessage& response,
        int timeoutMs) = 0;

    enum class AgentProbe { NO_AGENT,
        BUSY,
        OK,
        TIMEOUT };
    virtual AgentProbe probeAgent(int timeoutMs) = 0;
};

class IIpcClient {
public:
    using MessageHandler = std::function<void(const IPCMessage&)>;

    virtual ~IIpcClient() = default;

    virtual bool connect(int timeoutMs) = 0;
    virtual void disconnect() = 0;
    virtual bool sendMessage(const IPCMessage& message) = 0;
    // Must be called BEFORE connect(): connect() starts the reader thread.
    virtual void setMessageHandler(const MessageHandler& handler) = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
};

std::shared_ptr<IIpcServer> createIpcServer(const std::string& endpoint);
std::unique_ptr<IIpcClient> createIpcClient(const std::string& endpoint);

}
