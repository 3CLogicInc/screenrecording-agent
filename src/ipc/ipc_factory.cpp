#include "ipc/ipc_transport.hpp"

#ifdef _WIN32
#include "ipc/named_pipe.hpp"
#else
#include "ipc/unix_socket.hpp"
#include <cstdlib>
#endif

namespace screenrec::ipc {

namespace {

// On POSIX, SCREENREC_SOCKET_PATH overrides the endpoint so the service and agent can run unprivileged.
std::string resolveEndpoint(const std::string& endpoint) {
#ifdef _WIN32
    return endpoint;
#else
    // Read once at startup before any thread could call setenv. //NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char* override = std::getenv("SCREENREC_SOCKET_PATH");
        override != nullptr && *override != '\0') {
        return override;
    }
    return endpoint;
#endif
}

}

std::shared_ptr<IIpcServer> createIpcServer(const std::string& endpoint) {
#ifdef _WIN32
    return std::make_shared<NamedPipeServer>(resolveEndpoint(endpoint));
#else
    return std::make_shared<UnixSocketServer>(resolveEndpoint(endpoint));
#endif
}

std::unique_ptr<IIpcClient> createIpcClient(const std::string& endpoint) {
#ifdef _WIN32
    return std::make_unique<NamedPipeClient>(resolveEndpoint(endpoint));
#else
    return std::make_unique<UnixSocketClient>(resolveEndpoint(endpoint));
#endif
}

}
