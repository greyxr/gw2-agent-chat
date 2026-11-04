#include "opencode_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
    gw2chat::Connection connection;
    assert(gw2chat::ParseConnection(R"({"url":"http://127.0.0.1:4096","username":"opencode","password":"0123456789abcdef"})", connection));
    assert(connection.port == 4096 && connection.host == "127.0.0.1");
    assert(!gw2chat::ParseConnection(R"({"url":"http://localhost:4096","username":"opencode","password":"0123456789abcdef"})", connection));
    assert(!gw2chat::ParseConnection(R"({"url":"http://127.0.0.1:4096/path","username":"opencode","password":"0123456789abcdef"})", connection));
    assert(gw2chat::ParseConnection(R"({"url":"http://[::1]:4096","username":"opencode","password":"0123456789abcdef"})", connection));
    assert(connection.host == "::1");
    assert(!gw2chat::ParseConnection(R"({"url":"http://127.0.0.1:4096","username":"opencode","password":"short"})", connection));

    std::string id;
    assert(gw2chat::ParseSessionId(R"({"id":"ses_12345"})", id));
    assert(id == "ses_12345");
    assert(!gw2chat::ParseSessionId(R"({"id":"../../elsewhere"})", id));

    gw2chat::ClientResult message;
    assert(gw2chat::ParseAssistantReply(R"({"info":{"role":"assistant"},"parts":[{"type":"text","text":"hello "},{"type":"text","text":"world"}]})", message));
    assert(message.text == "hello world");
    assert(gw2chat::ParseAssistantReply(R"({"info":{"error":{"message":"failed"}},"parts":[]})", message));
    assert(!message.error.empty());
    const auto oversized = std::string("{\"info\":{},\"parts\":[{\"type\":\"text\",\"text\":\"") +
        std::string((1 << 20) + 1, 'x') + "\"}]}";
    assert(!gw2chat::ParseAssistantReply(oversized, message));
    assert(!gw2chat::ParseAssistantReply("not json", message));
}
