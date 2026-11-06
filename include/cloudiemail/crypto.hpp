#pragma once
#include <array>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cm {
using Bytes = std::vector<unsigned char>;
using Json = nlohmann::json;
void crypto_init();
std::string hex(std::span<const unsigned char> bytes);
Bytes unhex(std::string_view value);
std::string random_id();
std::string utc_now();
std::string mail_date();
class Secret {
    std::array<unsigned char, 32> value_{};

  public:
    Secret();
    ~Secret();
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
    Secret(Secret &&other) noexcept;
    Secret &operator=(Secret &&other) noexcept;
    unsigned char *data() {
        return value_.data();
    }
    const unsigned char *data() const {
        return value_.data();
    }
    std::span<const unsigned char> bytes() const {
        return value_;
    }
};
struct Identity {
    Json document;
    Secret private_key;
};
Identity create_identity(std::string_view email, std::string_view password);
Secret unlock_identity(const Json &document, std::string_view password);
Json change_password(const Json &document, std::string_view old_password, std::string_view new_password);
Bytes seal_message(const Json &document, std::string_view message);
std::string open_message(const Json &document, const Secret &private_key,
                         std::span<const unsigned char> ciphertext);
void wipe(std::string &value);
} // namespace cm
