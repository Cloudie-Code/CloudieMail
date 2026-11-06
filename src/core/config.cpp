#include "cloudiemail/config.hpp"
#include "cloudiemail/setup.hpp"
#include <fstream>
#include <regex>
#include <set>

namespace cm {
std::string path_utf8(const std::filesystem::path &path) {
    auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}
std::filesystem::path utf8_path(const std::string &value) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
void Config::save(const std::filesystem::path &path) const {
    validate();
    const auto base = std::filesystem::absolute(path).parent_path();
    auto relative = [&](const std::filesystem::path &p) {
        auto absolute = std::filesystem::absolute(p);
        auto relative_path = absolute.lexically_relative(base);
        return path_utf8(relative_path.empty() ? absolute : relative_path);
    };
    Json j = {{"domain", domain},
              {"bind", bind},
              {"webmail_driver", webmail_driver},
              {"language", language},
              {"webmail_backend_port", webmail_backend_port},
              {"submission_port", submission_port},
              {"outbound_enabled", outbound_enabled},
              {"outbound_ipv6", outbound_ipv6},
              {"antispam_enabled", antispam_enabled},
              {"antispam_dns", antispam_dns},
              {"antispam_threshold", antispam_threshold},
              {"antispam_blocklist", antispam_blocklist},
              {"database", relative(database)},
              {"certificate", relative(certificate)},
              {"private_key", relative(private_key)},
              {"web_root", relative(web_root)},
              {"database_user", database_user},
              {"database_password_env", database_password_env},
              {"smtp_port", smtp_port},
              {"smtps_port", smtps_port},
              {"imap_port", imap_port},
              {"pop3_port", pop3_port},
              {"https_port", https_port},
              {"max_message_bytes", max_message_bytes},
              {"max_connections", max_connections},
              {"mailbox_quota_bytes", mailbox_quota_bytes},
              {"session_timeout_seconds", session_timeout_seconds}};
    save_json(path, j);
}
std::string domain_name(std::string value) {
    for (auto &c : value) {
        if (static_cast<unsigned char>(c) > 127)
            throw std::runtime_error("Use dominio ASCII/punycode");
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const std::regex domain(
        R"(^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$)");
    if (value.size() > 253 || !std::regex_match(value, domain))
        throw std::runtime_error("Dominio invalido");
    return value;
}
std::string email_address(std::string value) {
    auto at = value.find('@');
    if (at == value.npos || at == 0 || at > 64 || value.size() > 254)
        throw std::runtime_error("Email invalido");
    auto local = value.substr(0, at);
    static const std::regex pattern(R"(^[A-Za-z0-9_+-]+(\.[A-Za-z0-9_+-]+)*$)");
    if (!std::regex_match(local, pattern))
        throw std::runtime_error("Parte local do email invalida");
    for (auto &c : local)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return local + "@" + domain_name(value.substr(at + 1));
}
Config Config::load(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Nao foi possivel abrir configuracao");
    Json j;
    file >> j;
    Config c;
    const std::set<std::string> allowed = {"domain",
                                           "bind",
                                           "webmail_driver",
                                           "language",
                                           "webmail_backend_port",
                                           "submission_port",
                                           "outbound_enabled",
                                           "outbound_ipv6",
                                           "antispam_enabled",
                                           "antispam_dns",
                                           "antispam_threshold",
                                           "antispam_blocklist",
                                           "database",
                                           "certificate",
                                           "private_key",
                                           "web_root",
                                           "database_user",
                                           "database_password_env",
                                           "smtp_port",
                                           "smtps_port",
                                           "imap_port",
                                           "pop3_port",
                                           "https_port",
                                           "max_message_bytes",
                                           "max_connections",
                                           "mailbox_quota_bytes",
                                           "session_timeout_seconds"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("Opcao desconhecida: " + it.key());
    c.domain = domain_name(j.value("domain", c.domain));
    c.bind = j.value("bind", c.bind);
    c.webmail_driver = j.value("webmail_driver", std::string("socket"));
    if (c.webmail_driver != "socket" && c.webmail_driver != "http_sys" && c.webmail_driver != "apache")
        throw std::runtime_error("Invalid webmail transport");
#ifdef _WIN32
    if (c.webmail_driver == "apache")
        throw std::runtime_error("Apache integration requires Linux");
#else
    if (c.webmail_driver == "http_sys")
        throw std::runtime_error("HTTP.sys esta disponivel somente no Windows");
#endif
    c.language = j.value("language", c.language);
    if (c.language != "pt-BR" && c.language != "en" && c.language != "ja" && c.language != "ko" &&
        c.language != "zh-TW")
        throw std::runtime_error("Unsupported language");
    c.outbound_enabled = j.value("outbound_enabled", c.outbound_enabled);
    c.outbound_ipv6 = j.value("outbound_ipv6", c.outbound_ipv6);
    c.antispam_enabled = j.value("antispam_enabled", c.antispam_enabled);
    c.antispam_dns = j.value("antispam_dns", c.antispam_dns);
    c.antispam_threshold = j.value("antispam_threshold", c.antispam_threshold);
    c.antispam_blocklist = j.value("antispam_blocklist", c.antispam_blocklist);
    auto base = std::filesystem::absolute(path).parent_path();
    auto resolve = [&](const char *key, const std::filesystem::path &fallback) {
        auto p = utf8_path(j.value(key, path_utf8(fallback)));
        return p.is_absolute() ? p : (base / p).lexically_normal();
    };
    c.database = resolve("database", c.database);
    c.certificate = resolve("certificate", c.certificate);
    c.private_key = resolve("private_key", c.private_key);
    c.web_root = resolve("web_root", c.web_root);
    c.database_user = j.value("database_user", c.database_user);
    c.database_password_env = j.value("database_password_env", c.database_password_env);
    auto port = [&](const char *key, unsigned short fallback) {
        auto n = j.value(key, int(fallback));
        if (n < 1 || n > 65535)
            throw std::runtime_error("Porta invalida");
        return static_cast<unsigned short>(n);
    };
    c.smtp_port = port("smtp_port", c.smtp_port);
    c.smtps_port = port("smtps_port", c.smtps_port);
    c.imap_port = port("imap_port", c.imap_port);
    c.pop3_port = port("pop3_port", c.pop3_port);
    c.https_port = port("https_port", c.https_port);
    c.webmail_backend_port = port("webmail_backend_port", c.webmail_backend_port);
    c.submission_port = port("submission_port", c.submission_port);
    for (auto listener : {c.smtp_port, c.smtps_port, c.imap_port, c.pop3_port, c.https_port})
        if (listener == c.submission_port ||
            (c.webmail_driver == "apache" && listener == c.webmail_backend_port))
            throw std::runtime_error("Ports must be distinct");
    if (c.webmail_driver == "apache" && c.submission_port == c.webmail_backend_port)
        throw std::runtime_error("Ports must be distinct");
    if (std::set<unsigned short>{c.smtp_port, c.smtps_port, c.imap_port, c.pop3_port, c.https_port}.size() !=
        5)
        throw std::runtime_error("Portas devem ser distintas");
    c.max_message_bytes = j.value("max_message_bytes", c.max_message_bytes);
    c.max_connections = j.value("max_connections", c.max_connections);
    c.mailbox_quota_bytes = j.value("mailbox_quota_bytes", c.mailbox_quota_bytes);
    c.session_timeout_seconds = j.value("session_timeout_seconds", c.session_timeout_seconds);
    if (c.max_message_bytes < 1024 || c.max_message_bytes > 25 * 1024 * 1024 || c.max_connections < 1 ||
        c.max_connections > 64 || c.session_timeout_seconds < 30 || c.session_timeout_seconds > 3600 ||
        c.mailbox_quota_bytes < c.max_message_bytes)
        throw std::runtime_error("Limites invalidos");
    c.validate();
    return c;
}
void Config::validate() const {
    if (antispam_threshold < 1 || antispam_threshold > 100 || antispam_blocklist.size() > 100)
        throw std::runtime_error("Invalid antispam settings");
    for (const auto &entry : antispam_blocklist) {
        if (entry.find('@') == std::string::npos)
            (void)domain_name(entry);
        else
            (void)email_address(entry);
    }
    (void)domain_name(domain);
    if (webmail_driver != "socket" && webmail_driver != "http_sys" && webmail_driver != "apache")
        throw std::runtime_error("Invalid webmail transport");
#ifdef _WIN32
    if (webmail_driver == "apache")
        throw std::runtime_error("Apache requires Linux");
#else
    if (webmail_driver == "http_sys")
        throw std::runtime_error("HTTP.sys requires Windows");
#endif
    if (language != "pt-BR" && language != "en" && language != "ja" && language != "ko" &&
        language != "zh-TW")
        throw std::runtime_error("Unsupported language");
    std::set<unsigned short> ports{smtp_port, smtps_port, imap_port, pop3_port, https_port, submission_port};
    if (ports.size() != 6 || ports.contains(0) ||
        (webmail_driver == "apache" &&
         (ports.contains(webmail_backend_port) || webmail_backend_port < 1024 || https_port != 443)))
        throw std::runtime_error("Invalid or conflicting ports");
    if (max_message_bytes < 1024 || max_message_bytes > 25 * 1024 * 1024 || max_connections < 1 ||
        max_connections > 64 || session_timeout_seconds < 30 || session_timeout_seconds > 3600 ||
        mailbox_quota_bytes < max_message_bytes || mailbox_quota_bytes > 64ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("Invalid resource limits");
}
} // namespace cm
