#include "opencode_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

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
    assert(message.text == "hello \n\nworld");
    assert(gw2chat::ParseAssistantReply(R"({"info":{},"parts":[{"type":"tool","tool":"x"},{"type":"text","text":""},{"type":"text","text":"only"}]})", message));
    assert(message.text == "only" && message.error.empty());
    assert(gw2chat::ParseAssistantReply(R"({"info":{"error":{"name":"APIError","data":{"message":"quota exceeded"}}},"parts":[]})", message));
    assert(message.error == "The agent reported an error: quota exceeded");
    for (const char* odd : {R"("failed")", R"({"data":[1]})", R"({"data":{"message":5}})", R"({"data":"x"})"}) {
        assert(gw2chat::ParseAssistantReply(std::string(R"({"info":{"error":)") + odd + R"(},"parts":[{"type":"text","text":"partial"}]})", message));
        assert(message.error == "The agent reported an error" && message.text == "partial");
    }
    assert(gw2chat::ParseAssistantReply(R"({"info":{"error":null},"parts":[]})", message) && message.error.empty());
    const auto longError = R"({"info":{"error":{"data":{"message":")" + std::string(299, 'x') + u8"\u732B" + R"("}}},"parts":[]})";
    assert(gw2chat::ParseAssistantReply(longError, message));
    assert(message.error == "The agent reported an error: " + std::string(299, 'x'));
    gw2chat::Connection same;
    assert(gw2chat::ParseConnection(R"({"url":"http://[::1]:4096","username":"opencode","password":"0123456789abcdef"})", same) && same == connection);
    assert(gw2chat::ParseConnection(R"({"url":"http://[::1]:4096","username":"opencode","password":"0123456789abcdeX"})", same) && !(same == connection));
    const auto oversized = std::string("{\"info\":{},\"parts\":[{\"type\":\"text\",\"text\":\"") +
        std::string((1 << 20) + 1, 'x') + "\"}]}";
    assert(!gw2chat::ParseAssistantReply(oversized, message));
    assert(!gw2chat::ParseAssistantReply("not json", message));
}
