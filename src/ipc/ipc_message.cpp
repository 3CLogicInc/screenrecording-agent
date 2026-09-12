#include "ipc/ipc_message.hpp"
#include <json.hpp>

using json = nlohmann::json;

namespace screenrec::ipc {

std::string serializeMessage(const IPCMessage& message) {
    json j;
    j["type"] = static_cast<int>(message.type);
    j["action"] = message.action;
    j["sessionId"] = message.sessionId;
    j["payload"] = message.payload;
    j["timestamp"] = message.timestamp;
    j["requestId"] = message.requestId;
    return j.dump();
}

IPCMessage deserializeMessage(const std::string& data) {
    IPCMessage message = {};
    try {
        json j = json::parse(data);
        message.type = static_cast<MessageType>(j["type"].get<int>());
        message.action = j["action"].get<std::string>();
        message.sessionId = j.value("sessionId", "");
        message.payload = j.value("payload", "");
        message.timestamp = j.value("timestamp", 0LL);
        message.requestId = j.value("requestId", "");
    } catch (const std::exception& e) {
        message.type = MessageType::ERROR_MSG;
        message.action = "parse_error";
        message.payload = e.what();
    } catch (...) {
        message.type = MessageType::ERROR_MSG;
        message.action = "parse_error";
        message.payload = "unknown exception during IPC message deserialization";
    }
    return message;
}

}
