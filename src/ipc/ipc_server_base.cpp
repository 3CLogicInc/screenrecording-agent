#include "ipc/ipc_server_base.hpp"
#include "ipc/protocol.hpp"

#include <chrono>

namespace screenrec::ipc {

void IpcServerBase::setMessageHandler(const MessageHandler& handler) {
    messageHandler_ = handler;
}

void IpcServerBase::registerAgent(const ClientId client) {
    std::scoped_lock lock(agentMutex_);
    connectedAgent_ = client;
}

void IpcServerBase::setAgentDisconnectHandler(const std::function<void()>& handler) {
    agentDisconnectHandler_ = handler;
}

void IpcServerBase::unregisterAgent(const ClientId client) {
    std::scoped_lock lock(agentMutex_);
    if (connectedAgent_ == client) {
        connectedAgent_ = INVALID_CLIENT;
    }
}

bool IpcServerBase::hasConnectedAgent() const {
    std::scoped_lock lock(agentMutex_);
    return connectedAgent_ != INVALID_CLIENT;
}

ClientId IpcServerBase::connectedAgent() const {
    std::scoped_lock lock(agentMutex_);
    return connectedAgent_;
}

bool IpcServerBase::sendToAgent(const IPCMessage& message) {
    const ClientId agent = connectedAgent();
    if (agent == INVALID_CLIENT) {
        return false;
    }
    return sendMessage(agent, message);
}

bool IpcServerBase::sendToAgentAndWait(const IPCMessage& message, IPCMessage& response,
    const int timeoutMs) {
    std::unique_lock<std::timed_mutex> inFlight(requestInFlightMutex_, std::defer_lock);
    if (!inFlight.try_lock_for(std::chrono::seconds(5))) {
        return false;
    }

    const ClientId agent = connectedAgent();
    if (agent == INVALID_CLIENT) {
        return false;
    }

    // only the reply echoing this id can satisfy the wait; setResponse() drops the rest
    IPCMessage request = message;
    request.requestId = std::to_string(++requestCounter_);

    {
        std::scoped_lock lock(responseMutex_);
        responseReceived_ = false;
        expectedRequestId_ = request.requestId;
    }

    if (!sendMessage(agent, request)) {
        std::scoped_lock lock(responseMutex_);
        expectedRequestId_.clear();
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(responseMutex_);
        const bool received = responseCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return responseReceived_ || !hasConnectedAgent();
        });
        expectedRequestId_.clear();

        if (received && responseReceived_) {
            response = lastResponse_;
            responseReceived_ = false;
            return true;
        }
    }

    return false;
}

IIpcServer::AgentProbe IpcServerBase::probeAgent(const int timeoutMs) {
    std::unique_lock<std::timed_mutex> inFlight(requestInFlightMutex_, std::defer_lock);
    if (!inFlight.try_lock_for(std::chrono::milliseconds(500))) {
        return AgentProbe::BUSY;
    }

    const ClientId agent = connectedAgent();
    if (agent == INVALID_CLIENT) {
        return AgentProbe::NO_AGENT;
    }

    IPCMessage request;
    request.type = MessageType::COMMAND;
    request.action = protocol::ACTION_GET_STATUS;
    request.requestId = std::to_string(++requestCounter_);
    request.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    {
        std::scoped_lock lock(responseMutex_);
        responseReceived_ = false;
        expectedRequestId_ = request.requestId;
    }

    if (!sendMessage(agent, request)) {
        std::scoped_lock lock(responseMutex_);
        expectedRequestId_.clear();
        return AgentProbe::TIMEOUT;
    }

    std::unique_lock<std::mutex> lock(responseMutex_);
    const bool received = responseCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return responseReceived_ || !hasConnectedAgent();
    });
    expectedRequestId_.clear();

    if (received && responseReceived_) {
        responseReceived_ = false;
        return AgentProbe::OK;
    }
    return hasConnectedAgent() ? AgentProbe::TIMEOUT : AgentProbe::NO_AGENT;
}

void IpcServerBase::handleIncomingMessage(const IPCMessage& message, const ClientId client) {
    // Wake the sendToAgentAndWait() waiter before running the handler, so the reply cannot be missed.
    if (message.type == MessageType::RESPONSE || message.type == MessageType::ERROR_MSG) {
        setResponse(message);
    }

    if (messageHandler_) {
        messageHandler_(message, client);
    }
}

void IpcServerBase::onClientDisconnected(const ClientId client) {
    bool wasRegisteredAgent = false;
    {
        std::scoped_lock lock(agentMutex_);
        if (connectedAgent_ == client) {
            connectedAgent_ = INVALID_CLIENT;
            wasRegisteredAgent = true;
        }
    }

    {
        std::scoped_lock lock(responseMutex_);
        responseCV_.notify_all();
    }

    // After the waiter wakeup, never before: the handler may join a thread that is itself parked in sendToAgentAndWait().
    if (wasRegisteredAgent && agentDisconnectHandler_) {
        agentDisconnectHandler_();
    }
}

void IpcServerBase::setResponse(const IPCMessage& response) {
    std::scoped_lock lock(responseMutex_);
    if (expectedRequestId_.empty() || response.requestId != expectedRequestId_) {
        return;
    }
    lastResponse_ = response;
    responseReceived_ = true;
    responseCV_.notify_all();
}

}
