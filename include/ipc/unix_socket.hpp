#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "ipc/client_connections.hpp"
#include "ipc/ipc_message.hpp"
#include "ipc/ipc_server_base.hpp"
#include "ipc/ipc_transport.hpp"

namespace screenrec::ipc {

// Wire format: 4-byte network-byte-order length prefix + UTF-8 JSON payload (stream sockets have no message boundaries).
class UnixSocketServer : public IpcServerBase {
public:
    explicit UnixSocketServer(std::string socketPath);
    ~UnixSocketServer() override;

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    bool start() override;
    void stop() override;

    bool sendMessage(ClientId client, const IPCMessage& message) override;

private:
    void acceptLoop();
    void readerThread(int fd, ClientId clientId);

    // Only root, the server's own user (unprivileged dev runs), or the current console user may connect; the analog of the Windows pipe DACL.
    [[nodiscard]] static bool peerAllowed(int fd);

    std::string socketPath_;
    int listenFd_ = -1;
    std::thread acceptThread_;

    ClientConnections<int> connections_;

    // Serializes frame writes AND guards the fd against a concurrent close: sendMessage holds it across lookup+write, the reader across erase+close.
    std::mutex sendMutex_;
};

class UnixSocketClient : public IIpcClient {
public:
    explicit UnixSocketClient(std::string socketPath);
    ~UnixSocketClient() override;

    UnixSocketClient(const UnixSocketClient&) = delete;
    UnixSocketClient& operator=(const UnixSocketClient&) = delete;

    bool connect(int timeoutMs) override;
    void disconnect() override;
    bool sendMessage(const IPCMessage& message) override;
    void setMessageHandler(const MessageHandler& handler) override;

    [[nodiscard]] bool isConnected() const override { return connected_; }

private:
    void readerThread();

    std::string socketPath_;
    std::atomic<int> fd_{-1}; // atomic: read by heartbeat/reader while disconnect() closes it
    std::atomic<bool> connected_{false};
    std::thread readerThread_;
    MessageHandler messageHandler_;
    std::mutex sendMutex_;
};

}
