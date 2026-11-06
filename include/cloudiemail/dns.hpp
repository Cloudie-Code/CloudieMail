#pragma once
#include "config.hpp"
namespace cm {
Json dns_plan(const std::string &domain, const std::string &ipv4);
std::string dns_txt_value(const std::string &value);
std::string dns_zone_text(const Json &plan);
Json apply_cloudflare(const std::string &zone_id, const std::string &token, const Json &plan);
std::string cloudflare_zone(const std::string &domain, const std::string &token);
} // namespace cm
