#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

#include "ipc/client_connections.hpp"
#include "ipc/ipc_message.hpp"
#include "ipc/ipc_server_base.hpp"
#include "ipc/ipc_transport.hpp"

namespace screenrec::ipc {

class NamedPipeServer : public IpcServerBase {
public:
    explicit NamedPipeServer(const std::string& pipeName);
    ~NamedPipeServer() override;

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    bool start() override;
    void stop() override;

    bool sendMessage(ClientId client, const IPCMessage& message) override;
    [[nodiscard]] std::uint32_t clientProcessId(ClientId client) const override;

private:
    void serverThread();
    void clientThread(HANDLE hPipe, ClientId clientId);
    [[nodiscard]] HANDLE createPipeInstance() const;
    static bool writeToPipe(HANDLE clientPipe, const IPCMessage& message);

    std::string pipeName_;
    std::thread serverThread_;
    HANDLE hStopEvent_;
    HANDLE hFirstPipe_ = INVALID_HANDLE_VALUE;

    ClientConnections<HANDLE> connections_;

    // Serializes writes AND guards a handle against a concurrent close: send holds it across lookup+write, the reader across erase+close.
    std::mutex sendMutex_;
};

class NamedPipeClient : public IIpcClient {
public:
    explicit NamedPipeClient(const std::string& pipeName);
    ~NamedPipeClient() override;

    NamedPipeClient(const NamedPipeClient&) = delete;
    NamedPipeClient& operator=(const NamedPipeClient&) = delete;

    bool connect(int timeoutMs) override;
    void disconnect() override;
    bool sendMessage(const IPCMessage& message) override;
    void setMessageHandler(const MessageHandler& handler) override;

    [[nodiscard]] bool isConnected() const override { return connected_; }

private:
    void readerThread();

    std::string pipeName_;
    HANDLE hPipe_;
    std::atomic<bool> connected_{false};
    std::thread readerThread_;
    MessageHandler messageHandler_;
};

}
