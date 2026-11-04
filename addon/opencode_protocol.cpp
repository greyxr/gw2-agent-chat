#include "opencode_protocol.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <regex>
#include <utility>

namespace gw2chat {
namespace {

bool SafeName(std::string_view value) {
    static const std::regex pattern(R"([A-Za-z0-9_.-]{1,64})");
    return std::regex_match(value.begin(), value.end(), pattern);
}

bool SafeSessionId(std::string_view value) {
    static const std::regex pattern(R"(ses[A-Za-z0-9_-]{1,125})");
    return std::regex_match(value.begin(), value.end(), pattern);
}

}

bool ParseConnection(std::string_view input, Connection& out) {
    try {
        const auto data = nlohmann::json::parse(input);
        const auto url = data.at("url").get<std::string>();
        constexpr std::string_view ipv4Prefix = "http://127.0.0.1:";
        constexpr std::string_view ipv6Prefix = "http://[::1]:";
        std::string host;
        std::string_view prefix;
        if (url.compare(0, ipv4Prefix.size(), ipv4Prefix) == 0) {
            host = "127.0.0.1";
            prefix = ipv4Prefix;
        } else if (url.compare(0, ipv6Prefix.size(), ipv6Prefix) == 0) {
            host = "::1";
            prefix = ipv6Prefix;
        } else return false;
        const auto portText = std::string_view(url).substr(prefix.size());
        unsigned port = 0;
        const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
        if (portText.empty() || parsed.ec != std::errc{} || parsed.ptr != portText.data() + portText.size() || port == 0 || port > 65535) return false;
        auto username = data.at("username").get<std::string>();
        auto password = data.at("password").get<std::string>();
        if (!SafeName(username) || password.size() < 16 || password.size() > 256) return false;
        for (unsigned char c : password) if (c < 0x20 || c > 0x7e) return false;
        out = {std::move(host), static_cast<std::uint16_t>(port), std::move(username), std::move(password)};
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseSessionId(std::string_view input, std::string& id) {
    try {
        auto candidate = nlohmann::json::parse(input).at("id").get<std::string>();
        if (!SafeSessionId(candidate)) return false;
        id = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseAssistantReply(std::string_view input, ClientResult& out) {
    try {
        const auto data = nlohmann::json::parse(input);
        ClientResult message;
        if (data.at("info").contains("error")) message.error = "The agent reported an error";
        for (const auto& part : data.at("parts")) {
            if (part.at("type").get<std::string>() == "text") {
                const auto piece = part.at("text").get<std::string>();
                if (message.text.size() + piece.size() > (1 << 20)) return false;
                message.text += piece;
            }
        }
        out = std::move(message);
        return true;
    } catch (...) {
        return false;
    }
}

std::string PromptBody(std::string_view text) {
    return nlohmann::json{{"agent", "gw2"}, {"parts", {{{"type", "text"}, {"text", text}}}}}.dump();
}

}
