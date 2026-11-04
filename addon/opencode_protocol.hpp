#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>

namespace gw2chat {

struct Connection {
    std::string host;
    std::uint16_t port = 0;
    std::string username;
    std::string password;
    explicit operator bool() const { return port != 0; }
};

struct ClientResult {
    std::string text;
    std::string error;
    bool offline = false;
};

bool ParseConnection(std::string_view json, Connection& out);
bool ParseSessionId(std::string_view json, std::string& id);
bool ParseAssistantReply(std::string_view json, ClientResult& out);
std::string PromptBody(std::string_view text);
ClientResult Prompt(const Connection& config, std::string_view text, std::string& sessionId,
                    const std::atomic<bool>& cancel);

}
