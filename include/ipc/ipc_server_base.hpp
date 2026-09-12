#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "ipc/ipc_transport.hpp"

namespace screenrec::ipc {

class IpcServerBase : public IIpcServer {
public:
    void setMessageHandler(const MessageHandler& handler) override;
    [[nodiscard]] bool isRunning() const override { return running_; }

    void registerAgent(ClientId client) override;
    void unregisterAgent(ClientId client) override;
    [[nodiscard]] bool hasConnectedAgent() const override;
    void setAgentDisconnectHandler(const std::function<void()>& handler) override;

    bool sendToAgent(const IPCMessage& message) override;
    bool sendToAgentAndWait(const IPCMessage& message, IPCMessage& response,
        int timeoutMs) override;
    AgentProbe probeAgent(int timeoutMs) override;

protected:
    [[nodiscard]] ClientId connectedAgent() const;

    void handleIncomingMessage(const IPCMessage& message, ClientId client);

    // Reader threads call this on disconnect so a pending sendToAgentAndWait() fails fast.
    void onClientDisconnected(ClientId client);

    void setRunning(const bool running) { running_ = running; }

private:
    void setResponse(const IPCMessage& response);

    std::atomic<bool> running_{false};
    MessageHandler messageHandler_;
    std::function<void()> agentDisconnectHandler_;

    mutable std::mutex agentMutex_;
    ClientId connectedAgent_ = INVALID_CLIENT;

    // Serializes sendToAgentAndWait(): there is only one response slot.
    std::timed_mutex requestInFlightMutex_;

    std::mutex responseMutex_;
    std::condition_variable responseCV_;
    bool responseReceived_ = false;
    IPCMessage lastResponse_;
    std::string expectedRequestId_; // in-flight request id, guarded by responseMutex_
    uint64_t requestCounter_ = 0;   // guarded by requestInFlightMutex_
};

}
