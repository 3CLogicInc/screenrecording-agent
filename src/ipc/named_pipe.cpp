#include "ipc/named_pipe.hpp"
#include <chrono>
#include <iostream>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace screenrec {
namespace ipc {

constexpr DWORD PIPE_BUFFER_SIZE = 65536;
constexpr DWORD PIPE_WRITE_TIMEOUT_MS = 30000;

namespace {

bool awaitOverlapped(const HANDLE pipe, const HANDLE stopEvent, OVERLAPPED& overlapped,
    const BOOL started, const DWORD timeoutMs, DWORD& bytes) {
    if (started) {
        return GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != 0;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return false;
    }

    const HANDLE handles[2] = {stopEvent, overlapped.hEvent};
    if (WaitForMultipleObjects(2, handles, FALSE, timeoutMs) == WAIT_OBJECT_0 + 1) {
        return GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != 0;
    }

    CancelIoEx(pipe, &overlapped);
    GetOverlappedResult(pipe, &overlapped, &bytes, TRUE);
    return false;
}

}

NamedPipeServer::NamedPipeServer(const std::string& pipeName)
    : pipeName_("\\\\.\\pipe\\" + pipeName),
      hStopEvent_(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {}

NamedPipeServer::~NamedPipeServer() {
    stop();
    if (hStopEvent_ != nullptr) {
        CloseHandle(hStopEvent_);
    }
}

bool NamedPipeServer::start() {
    if (isRunning()) {
        return false;
    }

    // First instance must exist before the server thread starts so agents can connect.
    const HANDLE hFirstPipe = createPipeInstance();

    if (hFirstPipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        std::cerr << "Failed to create initial named pipe. Error: " << error << "\n";
        return false;
    }

    hFirstPipe_ = hFirstPipe;

    setRunning(true);
    ResetEvent(hStopEvent_);
    serverThread_ = std::thread(&NamedPipeServer::serverThread, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return true;
}

void NamedPipeServer::stop() {
    if (!isRunning()) {
        return;
    }

    setRunning(false);
    SetEvent(hStopEvent_);

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    // Each reader thread closes its own pipe handle; joinAll waits for them.
    connections_.joinAll();
}

bool NamedPipeServer::sendMessage(const ClientId client, const IPCMessage& message) {
    std::scoped_lock lock(sendMutex_);
    const auto pipe = connections_.lookup(client);
    if (!pipe) {
        return false;
    }
    return writeToPipe(*pipe, message);
}

std::uint32_t NamedPipeServer::clientProcessId(const ClientId client) const {
    const auto pipe = connections_.lookup(client);
    if (!pipe || *pipe == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(*pipe, &pid) == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(pid);
}

bool NamedPipeServer::writeToPipe(const HANDLE clientPipe, const IPCMessage& message) {
    if (clientPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::string data = serializeMessage(message);
    DWORD bytesWritten = 0;

    // Use OVERLAPPED I/O to avoid blocking when another thread is reading
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    if (overlapped.hEvent == nullptr) {
        return false;
    }

    BOOL result = WriteFile(
        clientPipe,
        data.c_str(),
        static_cast<DWORD>(data.size()),
        &bytesWritten,
        &overlapped);

    if (!result) {
        if (const DWORD error = GetLastError(); error == ERROR_IO_PENDING) {
            const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 30000);
            if (waitResult == WAIT_OBJECT_0) {
                // A signaled event only means the write finished; success comes from GetOverlappedResult.
                result = GetOverlappedResult(clientPipe, &overlapped, &bytesWritten, FALSE);
            } else {
                // CancelIo only requests cancellation; block until it completes or the kernel may write I/O status into this freed stack OVERLAPPED.
                CancelIo(clientPipe);
                GetOverlappedResult(clientPipe, &overlapped, &bytesWritten, TRUE);
                CloseHandle(overlapped.hEvent);
                return false;
            }
        } else {
            CloseHandle(overlapped.hEvent);
            return false;
        }
    }

    CloseHandle(overlapped.hEvent);

    if (!result || bytesWritten != data.size()) {
        return false;
    }

    FlushFileBuffers(clientPipe);
    return true;
}

void NamedPipeServer::serverThread() {
    bool firstIteration = true;

    while (isRunning()) {
        HANDLE hPipe = nullptr;

        if (firstIteration && hFirstPipe_ != INVALID_HANDLE_VALUE) {
            hPipe = hFirstPipe_;
            hFirstPipe_ = INVALID_HANDLE_VALUE;
            firstIteration = false;
        } else {
            hPipe = createPipeInstance();

            if (hPipe == INVALID_HANDLE_VALUE) {
                Sleep(1000);
                continue;
            }
        }

        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        if (overlapped.hEvent == nullptr) {
            CloseHandle(hPipe);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, &overlapped);
        const DWORD error = GetLastError();

        if (!connected) {
            if (error == ERROR_IO_PENDING) {
                const HANDLE handles[2] = {hStopEvent_, overlapped.hEvent};
                const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

                if (wait == WAIT_OBJECT_0) {
                    CancelIo(hPipe);
                    CloseHandle(overlapped.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 1) {
                    connected = TRUE;
                }
            } else if (error == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }

        CloseHandle(overlapped.hEvent);

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        connections_.reapFinished();
        connections_.add(hPipe, [this, hPipe](const ClientId clientId) {
            clientThread(hPipe, clientId);
        });
    }
}

void NamedPipeServer::clientThread(const HANDLE hPipe, const ClientId clientId) {
    onClientConnected(clientId);
    char buffer[PIPE_BUFFER_SIZE];

    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    while (overlapped.hEvent != nullptr && isRunning()) {
        DWORD bytesRead = 0;
        ResetEvent(overlapped.hEvent);
        BOOL success = ReadFile(
            hPipe,
            buffer,
            sizeof(buffer) - 1,
            &bytesRead, &overlapped);

        if (!success) {
            if (GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            const HANDLE handles[2] = {hStopEvent_, overlapped.hEvent};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                CancelIo(hPipe);
                GetOverlappedResult(hPipe, &overlapped, &bytesRead, TRUE);
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1 ||
                GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE) == 0) {
                break;
            }
        }

        if (bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        std::string data(buffer, bytesRead);

        IPCMessage message = deserializeMessage(data);

        handleIncomingMessage(message, clientId);
    }

    if (overlapped.hEvent != nullptr) {
        CloseHandle(overlapped.hEvent);
    }

    // Erase + close under sendMutex_ so a concurrent sendMessage() cannot write to a closed handle.
    {
        std::scoped_lock lock(sendMutex_);
        connections_.erase(clientId);
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    onClientDisconnected(clientId);
}

HANDLE NamedPipeServer::createPipeInstance() const {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;

    // A world-writable pipe would let any local process register as the agent and hijack recordings: GRGW for SY/BA/IU only.
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GRGW;;;SY)(A;;GRGW;;;BA)(A;;GRGW;;;IU)",
            SDDL_REVISION_1,
            &pSD, nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    } else {
        std::cerr << "Failed to create security descriptor. Error: " << GetLastError() << "\n";
        sa.lpSecurityDescriptor = NULL;
    }

    const HANDLE hPipe = CreateNamedPipeA(
        pipeName_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        0,
        &sa);

    if (pSD != nullptr) {
        LocalFree(pSD);
    }

    return hPipe;
}

NamedPipeClient::NamedPipeClient(const std::string& pipeName)
    : pipeName_("\\\\.\\pipe\\" + pipeName), hPipe_(INVALID_HANDLE_VALUE),
      hStopEvent_(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {
}

NamedPipeClient::~NamedPipeClient() {
    disconnect();
    if (hStopEvent_ != nullptr) {
        CloseHandle(hStopEvent_);
    }
}

bool NamedPipeClient::connect(int timeoutMs) {
    if (connected_) {
        return true;
    }

    if (hStopEvent_ == nullptr) {
        return false;
    }

    if (!WaitNamedPipeA(pipeName_.c_str(), timeoutMs)) {
        return false;
    }

    const HANDLE pipe = CreateFileA(
        pipeName_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);

    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;

    if (const BOOL success = SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        !success) {
        CloseHandle(pipe);
        return false;
    }

    ResetEvent(hStopEvent_);
    {
        std::scoped_lock lock(sendMutex_);
        hPipe_ = pipe;
    }
    connected_ = true;
    readerThread_ = std::thread(&NamedPipeClient::readerThread, this);

    return true;
}

void NamedPipeClient::disconnect() {
    // Unconditional and idempotent: gating on connected_ would leak hPipe_ and leave readerThread_ joinable -> std::terminate().
    connected_ = false;

    if (hStopEvent_ != nullptr) {
        SetEvent(hStopEvent_);
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    std::scoped_lock lock(sendMutex_);
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }
}

bool NamedPipeClient::sendMessage(const IPCMessage& message) {
    if (!connected_) {
        return false;
    }

    std::scoped_lock lock(sendMutex_);
    if (hPipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::string data = serializeMessage(message);

    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return false;
    }

    const BOOL started = WriteFile(
        hPipe_,
        data.c_str(),
        static_cast<DWORD>(data.size()),
        nullptr, &overlapped);

    DWORD bytesWritten = 0;
    const bool completed = awaitOverlapped(hPipe_, hStopEvent_, overlapped, started, PIPE_WRITE_TIMEOUT_MS, bytesWritten);
    CloseHandle(overlapped.hEvent);

    return completed && bytesWritten == data.size();
}

void NamedPipeClient::setMessageHandler(const MessageHandler& handler) {
    messageHandler_ = handler;
}

void NamedPipeClient::readerThread() {
    char buffer[PIPE_BUFFER_SIZE];

    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    while (overlapped.hEvent != nullptr && connected_) {
        const BOOL started = ReadFile(
            hPipe_,
            buffer,
            sizeof(buffer) - 1,
            nullptr, &overlapped);

        DWORD bytesRead = 0;
        if (!awaitOverlapped(hPipe_, hStopEvent_, overlapped, started, INFINITE, bytesRead) || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        std::string data(buffer, bytesRead);

        IPCMessage message = deserializeMessage(data);

        if (messageHandler_) {
            messageHandler_(message);
        }
    }

    connected_ = false;

    if (overlapped.hEvent != nullptr) {
        CloseHandle(overlapped.hEvent);
    }
}

}
}
