#include "cloudiemail/wizard.hpp"
#include <fstream>
#include <iostream>

int main() {
    try {
        cm::crypto_init();
        auto root = std::filesystem::temp_directory_path() / ("cloudiemail-config-" + cm::random_id());
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{root};
        unsigned checks = 0;
        auto check = [&](bool value, const char *message) {
            if (!value)
                throw std::runtime_error(message);
            ++checks;
        };
        auto path = root / "config/local.json";
        cm::Setup fresh(path);
#ifdef _WIN32
        check(fresh.config.https_port == 443 && fresh.config.webmail_driver == "http_sys",
              "Windows defaults");
        cm::Wizard wizard(path);
        bool checkbox = false;
        for (const auto &field : wizard.view().fields)
            if (field.id == "http_sys")
                checkbox = field.checkbox && field.value == "1";
        check(checkbox, "Native HTTPS is not selected in fresh Windows wizard");
#else
        check(fresh.config.https_port == 8443 && fresh.config.webmail_driver == "socket",
              "Linux defaults changed");
#endif
        cm::save_json(path, {{"domain", "example.test"}, {"https_port", 9443}});
        auto legacy = cm::Config::load(path);
        check(!fresh.config.outbound_ipv6 && !legacy.outbound_ipv6,
              "Fresh and legacy configurations must use IPv4 for outgoing SMTP by default");
        check(legacy.webmail_driver == "socket" && legacy.https_port == 9443,
              "Legacy install changed transport or port");
        cm::Setup resumed(path);
        check(resumed.config.webmail_driver == "socket" && resumed.config.https_port == 9443,
              "Wizard changed legacy config");
        legacy.save(path);
        check(cm::Config::load(path).webmail_driver == "socket", "Transport not persisted");
        legacy.outbound_ipv6 = true;
        legacy.save(path);
        check(cm::Config::load(path).outbound_ipv6, "Explicit IPv6 opt-in not persisted");
        legacy.outbound_ipv6 = false;
        legacy.save(path);
        check(!cm::Config::load(path).outbound_ipv6, "IPv4-only choice not persisted");
        check(legacy.antispam_enabled && legacy.antispam_dns && legacy.antispam_threshold == 5,
              "Legacy settings do not enable antispam defaults");
        legacy.antispam_threshold = 7;
        legacy.antispam_blocklist = {"blocked.example", "spam@blocked.example"};
        legacy.save(path);
        auto spam_saved = cm::Config::load(path);
        check(spam_saved.antispam_threshold == 7 &&
                  spam_saved.antispam_blocklist == legacy.antispam_blocklist,
              "Antispam settings not persisted");
        cm::Wizard preferences(path);
        preferences.perform("preferences", {});
        bool ipv6_checkbox = false;
        for (const auto &field : preferences.view().fields)
            if (field.id == "outbound_ipv6")
                ipv6_checkbox = field.checkbox && field.value == "0";
        check(ipv6_checkbox, "IPv6 delivery must be an explicit opt-in in preferences");
        bool spam_checkbox = false;
        for (const auto &field : preferences.view().fields)
            if (field.id == "antispam")
                spam_checkbox = field.checkbox && field.value == "1";
        check(spam_checkbox, "Antispam setting not exposed in GUI/console");
        for (const auto &driver : {"invalid", "http_sys"}) {
            cm::save_json(path, {{"webmail_driver", driver}});
            bool accepted = false;
            try {
                accepted = cm::Config::load(path).webmail_driver == driver;
            } catch (const std::exception &) {
            }
#ifdef _WIN32
            check(accepted == (std::string(driver) == "http_sys"), "Incorrect Windows transport validation");
#else
            check(!accepted, "Unsupported Linux transport accepted");
#endif
        }
        std::cout << checks << " config compatibility checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
