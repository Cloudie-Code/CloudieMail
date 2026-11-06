#pragma once
#include "database.hpp"
#include <atomic>
#include <functional>
#include <map>

namespace cm {
using Progress = std::function<void(const std::string &)>;
void private_directory(const std::filesystem::path &path);
void save_json(const std::filesystem::path &path, const Json &value);
void validate_certificate(const Config &config);
std::filesystem::path executable_directory();
int run_child(const std::filesystem::path &binary, const std::vector<std::string> &arguments,
              const std::map<std::string, std::string> &environment, const Progress &output,
              std::atomic_bool &cancel, std::atomic_uint &continue_input);

enum class SetupStep { Domain, Dns, Certificate, Account, Ready, Preferences, Accounts };
class Setup {
    std::filesystem::path path_, state_path_;
    Json state_ = Json::object();
    void save_state();

  public:
    Config config;
    explicit Setup(std::filesystem::path path);
    SetupStep step();
    const std::filesystem::path &path() const {
        return path_;
    }
    std::string ipv4() const {
        return state_.value("ipv4", "");
    }
    std::string contact() const {
        return state_.value("contact", "");
    }
    std::string dns_mode() const {
        return state_.value("dns_mode", "manual");
    }
    void configure(const std::string &domain, const std::string &ipv4, const std::string &bind,
                   const std::vector<unsigned short> &ports, const std::string &webmail_driver = "");
    std::string dns_records() const;
    void dns_manual_confirmed();
    void dns_cloudflare(const std::string &token);
    void import_certificate(const std::filesystem::path &certificate, const std::filesystem::path &key);
    void configure_webhost(const Progress &output, std::atomic_bool &cancel, std::atomic_uint &input);
    void issue_certificate(const std::string &contact, const std::string &provider, const std::string &token,
                           const Progress &output, std::atomic_bool &cancel, std::atomic_uint &continue_input,
                           bool force = false);
    void first_account(const std::string &email, std::string password, std::string confirmation);
};
void setup_console(const std::filesystem::path &path, bool manage = false);
} // namespace cm
