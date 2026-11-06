#pragma once
#include "crypto.hpp"
namespace cm {
std::vector<std::string> parse_recipients(const std::string &text);
std::string compose_message(const std::string &sender, const std::vector<std::string> &recipients,
                            const Json &data, const std::string &domain);
} // namespace cm
