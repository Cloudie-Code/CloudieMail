#include "cloudiemail/i18n.hpp"
#include "cloudiemail/crypto.hpp"
#include "translations.hpp"
#include <mutex>
namespace cm {
namespace {
std::mutex language_mutex;
std::string language_code = "pt-BR";
const Json &catalog() {
    static const Json value = Json::parse(reinterpret_cast<const char *>(cloudiemail_translations));
    return value;
}
} // namespace
void set_language(const std::string &language) {
    if (!catalog().contains(language))
        throw std::runtime_error("Unsupported language");
    std::lock_guard lock(language_mutex);
    language_code = language;
}
std::string current_language() {
    std::lock_guard lock(language_mutex);
    return language_code;
}
std::string tr(std::string_view key, const std::string &language) {
    const auto &dictionary = catalog().at(language.empty() ? current_language() : language);
    auto found = dictionary.find(std::string(key));
    return found == dictionary.end() ? std::string(key) : found->get<std::string>();
}
} // namespace cm
