#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace gw2chat {

struct SSEEvent {
    std::string name;
    std::string data;
};

class SSEParser {
public:
    using Handler = std::function<void(SSEEvent&&)>;

    explicit SSEParser(Handler handler, std::size_t maxEventBytes = 1 << 20);
    bool Feed(std::string_view bytes);

private:
    bool Line(std::string_view line);
    void Dispatch();

    Handler handler_;
    std::size_t maxEventBytes_;
    std::string pending_;
    SSEEvent event_;
    std::size_t dataBytes_ = 0;
    bool hasData_ = false;
    bool failed_ = false;
};

}
