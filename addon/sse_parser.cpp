#include "sse_parser.hpp"

#include <utility>

namespace gw2chat {

SSEParser::SSEParser(Handler handler, std::size_t maxEventBytes)
    : handler_(std::move(handler)), maxEventBytes_(maxEventBytes) {}

bool SSEParser::Feed(std::string_view bytes) {
    if (failed_) return false;
    std::size_t start = 0;
    for (;;) {
        const auto end = bytes.find('\n', start);
        const auto count = end == std::string_view::npos ? bytes.size() - start : end - start;
        if (pending_.size() + count > maxEventBytes_ + 128) {
            failed_ = true;
            return false;
        }
        pending_.append(bytes.data() + start, count);
        if (end == std::string_view::npos) break;
        auto line = std::string_view(pending_);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!Line(line)) {
            failed_ = true;
            return false;
        }
        pending_.clear();
        start = end + 1;
    }
    return true;
}

bool SSEParser::Line(std::string_view line) {
    if (line.empty()) {
        Dispatch();
        return true;
    }
    if (line.front() == ':') return true;
    const auto colon = line.find(':');
    const auto field = line.substr(0, colon);
    auto value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (field == "event") {
        if (value.size() > 64) return false;
        event_.name.assign(value);
    } else if (field == "data") {
        if (dataBytes_ + value.size() + (hasData_ ? 1 : 0) > maxEventBytes_) return false;
        if (hasData_) event_.data.push_back('\n');
        event_.data.append(value);
        dataBytes_ += value.size() + (hasData_ ? 1 : 0);
        hasData_ = true;
    }
    return true;
}

void SSEParser::Dispatch() {
    if (!event_.name.empty() || !event_.data.empty()) handler_(std::move(event_));
    event_ = {};
    dataBytes_ = 0;
    hasData_ = false;
}

}
