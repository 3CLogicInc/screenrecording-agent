#pragma once

#include <cstdint>
#include <string>

namespace screenrec::ipc {

enum class MessageType {
    COMMAND,
    RESPONSE,
    HEARTBEAT,
    ERROR_MSG,
    UPLOAD_REQUEST,
    LOG_ENTRY,
    STATUS_QUERY,
    MANIFEST_REQUEST
};

struct IPCMessage {
    MessageType type;
    std::string action;
    std::string sessionId;
    std::string payload;
    int64_t timestamp{};
    std::string requestId; // correlates replies with sendToAgentAndWait(); empty on spontaneous messages
};

std::string serializeMessage(const IPCMessage& message);
IPCMessage deserializeMessage(const std::string& data);

}
