#pragma once
#include "config.hpp"
#include <functional>
namespace cm {
using SpamDns = std::function<std::vector<std::string>(const std::string &, unsigned)>;
struct SpamResult {
    int score = 0;
    bool junk = false;
    std::string spf = "none", dkim = "none", dmarc = "none";
    std::vector<std::string> rules;
    std::string headers() const;
};
SpamResult classify_spam(const Config &config, std::string_view message, const std::string &sender,
                         const std::string &peer, const std::string &helo, SpamDns resolver = {});
std::string remove_spam_headers(std::string_view message);
} // namespace cm
