#include "sse_parser.hpp"

#include <cassert>
#include <string>
#include <vector>

int main() {
    std::vector<gw2chat::SSEEvent> events;
    gw2chat::SSEParser parser([&](gw2chat::SSEEvent&& event) { events.push_back(std::move(event)); });
    assert(parser.Feed(": heartbeat\r\nevent: message\r\ndata: first\r\n"));
    assert(parser.Feed("data: second\r\n\r\nevent: done\ndata:\n\n"));
    assert(events.size() == 2);
    assert(events[0].name == "message");
    assert(events[0].data == "first\nsecond");
    assert(events[1].name == "done");
    assert(events[1].data.empty());

    std::string unicode;
    gw2chat::SSEParser split([&](gw2chat::SSEEvent&& event) { unicode = std::move(event.data); });
    assert(split.Feed("event: message\ndata: caf\xc3"));
    assert(split.Feed("\xa9\n\n"));
    assert(unicode == "caf\xc3\xa9");

    bool overflowEvent = false;
    gw2chat::SSEParser bounded([&](gw2chat::SSEEvent&&) {}, 8);
    assert(bounded.Feed("data: "));
    overflowEvent = !bounded.Feed("123456789\n\n");
    assert(overflowEvent);
}
