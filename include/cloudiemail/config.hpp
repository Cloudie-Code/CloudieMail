#pragma once
#include "crypto.hpp"
#include <filesystem>

namespace cm {
std::string domain_name(std::string value);
std::string email_address(std::string value);
std::string path_utf8(const std::filesystem::path &path);
std::filesystem::path utf8_path(const std::string &value);
struct Config {
    std::string domain = "example.test", bind = "127.0.0.1";
    std::string webmail_driver = "socket";
    std::string language = "pt-BR";
    unsigned short webmail_backend_port = 8787, submission_port = 2587;
    bool outbound_enabled = true;
    bool outbound_ipv6 = false;
    std::filesystem::path database = "data/cloudiemail.fdb", certificate = "data/tls.crt",
                          private_key = "data/tls.key", web_root = "web";
    std::string database_user = "SYSDBA", database_password_env = "CLOUDIEMAIL_DB_PASSWORD";
    unsigned short smtp_port = 2525, smtps_port = 2465, imap_port = 2993, pop3_port = 2995, https_port = 8443;
    std::size_t max_message_bytes = 10 * 1024 * 1024, max_connections = 16,
                mailbox_quota_bytes = 256 * 1024 * 1024;
    unsigned session_timeout_seconds = 300;
    std::string mail_host() const {
        return "mail." + domain;
    }
    std::string webmail_host() const {
        return "webmail." + domain;
    }
    static Config load(const std::filesystem::path &path);
    void save(const std::filesystem::path &path) const;
    void validate() const;
};
} // namespace cm
