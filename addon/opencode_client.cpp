#include "opencode_protocol.hpp"

#include <httplib.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>

namespace gw2chat {
namespace {

constexpr std::size_t responseLimit = 2 << 20;

std::string Url(const Connection& config) {
    return "http://" + (config.host == "::1" ? "[::1]" : config.host) + ":" + std::to_string(config.port);
}

void Configure(httplib::Client& client, const Connection& config, int timeout) {
    client.set_basic_auth(config.username, config.password);
    client.set_follow_location(false);
    client.set_connection_timeout(3);
    client.set_write_timeout(3);
    client.set_read_timeout(timeout);
}

struct HttpResult {
    std::string body;
    std::string error;
    bool offline = false;
};

HttpResult Request(httplib::Client& client, const std::string& method, const std::string& path,
                   const std::string& body) {
    httplib::Request request;
    request.method = method;
    request.path = path;
    request.body = body;
    if (!body.empty()) request.set_header("Content-Type", "application/json");
    bool tooLarge = false;
    HttpResult result;
    request.content_receiver = [&](const char* data, std::size_t size, std::uint64_t, std::uint64_t) {
        if (result.body.size() + size > responseLimit) {
            tooLarge = true;
            return false;
        }
        result.body.append(data, size);
        return true;
    };
    const auto response = client.send(request);
    if (tooLarge) return {{}, "OpenCode response exceeded its size limit"};
    if (!response) return {{}, "OpenCode request failed", true};
    if (response->status < 200 || response->status >= 300)
        return {{}, "OpenCode returned HTTP " + std::to_string(response->status)};
    return result;
}

HttpResult Abort(const Connection& config, const std::string& sessionId) {
    httplib::Client client(Url(config));
    Configure(client, config, 3);
    auto pending = std::async(std::launch::async, [&] { return Request(client, "POST", "/session/" + sessionId + "/abort", {}); });
    if (pending.wait_for(std::chrono::seconds(3)) != std::future_status::ready) client.stop();
    return pending.get();
}

}

ClientResult Prompt(const Connection& config, std::string_view text, std::string& sessionId,
                    const std::atomic<bool>& cancel) {
    httplib::Client client(Url(config));
    Configure(client, config, 3);
    if (!client.is_valid()) return {{}, "Invalid OpenCode loopback URL", true};
    if (sessionId.empty()) {
        const auto created = Request(client, "POST", "/session", R"({"agent":"gw2"})");
        if (!created.error.empty()) return {{}, created.error, created.offline};
        if (!ParseSessionId(created.body, sessionId)) return {{}, "OpenCode returned an invalid session", true};
    }
    if (cancel.load()) return {};
    client.set_read_timeout(180);

    const auto path = "/session/" + sessionId + "/message";
    const auto payload = PromptBody(text);
    auto pending = std::async(std::launch::async, [&] { return Request(client, "POST", path, payload); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    while (pending.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready &&
           !cancel.load() && std::chrono::steady_clock::now() < deadline) {}
    if (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        client.stop();
        pending.wait();
        Abort(config, sessionId);
        return {{}, cancel.load() ? "Canceled" : "OpenCode did not finish the reply before the time limit"};
    }
    const auto response = pending.get();
    if (!response.error.empty()) return {{}, response.error, response.offline};
    ClientResult message;
    if (!ParseAssistantReply(response.body, message)) return {{}, "OpenCode returned an invalid reply", true};
    if (!message.error.empty()) return message;
    if (message.text.empty()) return {{}, "The agent returned an empty reply"};
    return message;
}

}
