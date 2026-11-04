#include "opencode_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <charconv>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    assert(argc == 2);
    unsigned port = 0;
    const std::string portText(argv[1]);
    const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    assert(parsed.ec == std::errc{} && parsed.ptr == portText.data() + portText.size() && port > 0 && port <= 65535);
    const gw2chat::Connection connection{"127.0.0.1", static_cast<std::uint16_t>(port), "opencode", "gw2-agent-chat-test-password"};
    std::string session;
    std::atomic<bool> cancel{false};
    auto first = gw2chat::Prompt(connection, "first", session, cancel);
    assert(first.error.empty() && first.text == "reply one" && session == "ses_fixture");
    auto second = gw2chat::Prompt(connection, u8"a\"b\u732B", session, cancel);
    assert(second.error.empty() && second.text == "reply two" && session == "ses_fixture");
    auto failed = gw2chat::Prompt(connection, "error", session, cancel);
    assert(failed.error == "OpenCode returned HTTP 400");
    auto large = gw2chat::Prompt(connection, "large", session, cancel);
    assert(large.error == "OpenCode response exceeded its size limit");

    gw2chat::ClientResult canceled;
    std::thread request([&] { canceled = gw2chat::Prompt(connection, "cancel", session, cancel); });
    std::cin.get();
    cancel.store(true);
    request.join();
    assert(canceled.error == "Canceled");
}
