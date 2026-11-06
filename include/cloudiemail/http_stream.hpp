#pragma once
#include <string>
#include <string_view>
#include <utility>

namespace cm {
class HttpStream {
  public:
    std::string peer;
    explicit HttpStream(std::string address) : peer(std::move(address)) {}
    virtual ~HttpStream() = default;
    virtual std::string line(std::size_t limit = 8192) = 0;
    virtual std::string read(std::size_t bytes) = 0;
    virtual void send_response(int status, std::string_view reason, std::string_view headers,
                               std::string_view body) = 0;
};
} // namespace cm
