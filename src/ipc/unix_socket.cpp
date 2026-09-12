#include "ipc/unix_socket.hpp"

#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace screenrec::ipc {

namespace {

// Largest accepted frame; anything bigger is a corrupt or hostile stream and drops the connection.
constexpr uint32_t MAX_FRAME_BYTES = 1024 * 1024;

constexpr int ACCEPT_POLL_MS = 500;

constexpr int CLIENT_CONNECT_RETRY_MS = 500;

// Thread-safe strerror (plain strerror shares a static buffer).
std::string errnoText() {
    std::array<char, 128> buf = {};
    (void)strerror_r(errno, buf.data(), buf.size());
    return buf.data();
}

void setNoSigPipe(const int fd) {
#ifdef SO_NOSIGPIPE
    // macOS has no MSG_NOSIGNAL: without this, a peer crashing mid-send() kills the process with SIGPIPE.
    constexpr int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool writeAll(const int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = send(fd, p, len, sendFlags());
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool readAll(const int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        const ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool writeFrame(const int fd, const std::string& payload) {
    if (payload.size() > MAX_FRAME_BYTES) {
        return false;
    }
    const uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    return writeAll(fd, &len, sizeof(len)) && writeAll(fd, payload.data(), payload.size());
}

// False on EOF, error, or a bad length: a corrupt length prefix cannot be resynced, so the caller must drop the connection.
bool readFrame(const int fd, std::string& payload) {
    uint32_t lenBE = 0;
    if (!readAll(fd, &lenBE, sizeof(lenBE))) {
        return false;
    }
    const uint32_t len = ntohl(lenBE);
    if (len == 0 || len > MAX_FRAME_BYTES) {
        return false;
    }
    payload.resize(len);
    return readAll(fd, payload.data(), len);
}

}

UnixSocketServer::UnixSocketServer(std::string socketPath)
    : socketPath_(std::move(socketPath)) {}

UnixSocketServer::~UnixSocketServer() {
    stop();
}

bool UnixSocketServer::start() {
    if (isRunning()) {
        return false;
    }

    // The parent directory may not exist yet (/var/run is wiped at boot); 0755 so only the daemon owner creates sockets in it.
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(socketPath_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Failed to create socket directory " << parent.string() << ": "
                      << ec.message() << "\n";
            return false;
        }
        chmod(parent.c_str(), 0755);
    }

    // bind() fails on an existing path, so clear a stale socket left by a previous run.
    unlink(socketPath_.c_str());

    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "Failed to create Unix socket: " << errnoText() << "\n";
        return false;
    }
    setNoSigPipe(listenFd_);

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Socket path too long: " << socketPath_ << "\n";
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Failed to bind " << socketPath_ << ": " << errnoText() << "\n";
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 0666 on purpose: authorization is per connection via peerAllowed(), not file permissions.
    chmod(socketPath_.c_str(), 0666);

    if (listen(listenFd_, 4) != 0) {
        std::cerr << "Failed to listen on " << socketPath_ << ": " << errnoText() << "\n";
        close(listenFd_);
        listenFd_ = -1;
        unlink(socketPath_.c_str());
        return false;
    }

    setRunning(true);
    acceptThread_ = std::thread(&UnixSocketServer::acceptLoop, this);
    return true;
}

void UnixSocketServer::stop() {
    if (!isRunning()) {
        return;
    }

    setRunning(false);

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    unlink(socketPath_.c_str());

    // Wake blocked readers with EOF before joining them; each reader closes its own fd.
    for (const int fd : connections_.handles()) {
        shutdown(fd, SHUT_RDWR);
    }
    connections_.joinAll();
}

bool UnixSocketServer::sendMessage(const ClientId client, const IPCMessage& message) {
    std::scoped_lock lock(sendMutex_);
    const auto fd = connections_.lookup(client);
    if (!fd) {
        return false;
    }
    return writeFrame(*fd, serializeMessage(message));
}

bool UnixSocketServer::peerAllowed(const int fd) {
    uid_t euid = 0;
    gid_t egid = 0;
#if defined(__APPLE__) || defined(__FreeBSD__)
    if (getpeereid(fd, &euid, &egid) != 0) {
        return false;
    }
#elif defined(SO_PEERCRED)
    ucred cred = {};
    socklen_t credLen = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0) {
        return false;
    }
    euid = cred.uid;
    egid = cred.gid;
#else
    return false;
#endif
    (void)egid;

    if (euid == 0 || euid == geteuid()) {
        return true;
    }

    // The current console (GUI) user owns /dev/console; only their agent may connect.
    struct stat consoleStat = {};
    return stat("/dev/console", &consoleStat) == 0 && consoleStat.st_uid == euid;
}

void UnixSocketServer::acceptLoop() {
    while (isRunning()) {
        pollfd pfd = {};
        pfd.fd = listenFd_;
        pfd.events = POLLIN;

        const int ready = poll(&pfd, 1, ACCEPT_POLL_MS);
        if (!isRunning()) {
            break;
        }
        if (ready <= 0) {
            continue;
        }

        const int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }

        if (!peerAllowed(fd)) {
            std::cerr << "IPC connection rejected: unauthorized peer\n";
            close(fd);
            continue;
        }

        setNoSigPipe(fd);

        connections_.reapFinished();
        connections_.add(fd, [this, fd](const ClientId clientId) {
            readerThread(fd, clientId);
        });
    }
}

void UnixSocketServer::readerThread(const int fd, const ClientId clientId) {
    std::string payload;

    while (isRunning()) {
        if (!readFrame(fd, payload)) {
            break;
        }

        const IPCMessage message = deserializeMessage(payload);

        handleIncomingMessage(message, clientId);
    }

    // Erase + close under sendMutex_ so a concurrent sendMessage() cannot write to a closed (and recycled) fd.
    {
        std::scoped_lock lock(sendMutex_);
        connections_.erase(clientId);
        close(fd);
    }

    onClientDisconnected(clientId);
}

UnixSocketClient::UnixSocketClient(std::string socketPath)
    : socketPath_(std::move(socketPath)) {}

UnixSocketClient::~UnixSocketClient() {
    disconnect();
}

bool UnixSocketClient::connect(const int timeoutMs) {
    if (connected_) {
        return true;
    }

    // ENOENT/ECONNREFUSED are expected while the daemon is still booting, so retry until the deadline.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            if (socketPath_.size() >= sizeof(addr.sun_path)) {
                close(fd);
                return false;
            }
            strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

            if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
                setNoSigPipe(fd);
                fd_.store(fd);
                connected_ = true;
                readerThread_ = std::thread(&UnixSocketClient::readerThread, this);
                return true;
            }
            close(fd);
        }

        if (std::chrono::steady_clock::now() +
                std::chrono::milliseconds(CLIENT_CONNECT_RETRY_MS) >=
            deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CLIENT_CONNECT_RETRY_MS));
    }
}

void UnixSocketClient::disconnect() {
    // Unconditional and idempotent: the reader clears connected_ itself, and a joinable readerThread_ at destruction calls std::terminate().
    connected_ = false;

    // shutdown() (not close()) wakes a blocked recv() without freeing the fd number, so nothing can recycle it under the reader.
    if (const int fd = fd_.load(); fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    // Close only after the reader stopped, under sendMutex_ so a concurrent sendMessage() sees a valid fd or -1, never a recycled one.
    std::scoped_lock lock(sendMutex_);
    if (const int fd = fd_.exchange(-1); fd >= 0) {
        close(fd);
    }
}

bool UnixSocketClient::sendMessage(const IPCMessage& message) {
    if (!connected_) {
        return false;
    }
    std::scoped_lock lock(sendMutex_);
    const int fd = fd_.load();
    if (fd < 0) {
        return false;
    }
    return writeFrame(fd, serializeMessage(message));
}

void UnixSocketClient::setMessageHandler(const MessageHandler& handler) {
    messageHandler_ = handler;
}

void UnixSocketClient::readerThread() {
    std::string payload;
    // fd_ is stable for the reader's lifetime: set before this thread starts, cleared by disconnect() only after it joins.
    const int fd = fd_.load();

    while (connected_) {
        if (!readFrame(fd, payload)) {
            connected_ = false;
            break;
        }

        const IPCMessage message = deserializeMessage(payload);

        if (messageHandler_) {
            messageHandler_(message);
        }
    }
}

}
