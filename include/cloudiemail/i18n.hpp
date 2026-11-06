#pragma once
#include <string>
#include <string_view>
namespace cm {
void set_language(const std::string &language);
std::string current_language();
std::string tr(std::string_view key, const std::string &language = "");
} // namespace cm
