#include "cloudiemail/wizard.hpp"
#include "cloudiemail/i18n.hpp"
#include <cstdlib>
#include <sstream>
namespace cm {
Wizard::Wizard(const std::filesystem::path &path) : setup_(path) {
    if (const char *language = std::getenv("CLOUDIEMAIL_LANGUAGE"))
        setup_.config.language = language;
    set_language(setup_.config.language);
    form(setup_.step());
}
Wizard::~Wizard() {
    cancel_ = true;
    if (worker_.joinable())
        worker_.join();
}
WizardView Wizard::view() {
    std::lock_guard lock(mutex_);
    return view_;
}
void Wizard::language(const std::string &code) {
    std::lock_guard lock(mutex_);
    if (view_.busy)
        return;
    set_language(code);
    setup_.config.language = code;
    setup_.config.save(setup_.path());
    form(view_.step);
}
void Wizard::form(SetupStep step) {
    view_.step = step;
    view_.fields.clear();
    view_.actions.clear();
    ++view_.revision;
    auto field = [&](std::string id, std::string label, std::string value = "", bool secret = false,
                     bool checkbox = false, bool file = false) {
        view_.fields.push_back({id, tr(label), value, secret, checkbox, file, {}});
    };
    auto action = [&](std::string id, std::string label) { view_.actions.push_back({id, tr(label)}); };
    switch (step) {
    case SetupStep::Domain:
        view_.title = tr("setup.domain_title");
        view_.body = tr("setup.domain_body");
        field("domain", "field.domain", setup_.config.domain == "example.test" ? "" : setup_.config.domain);
        field("ipv4", "field.ipv4", setup_.ipv4());
        field("bind", "field.bind", setup_.config.bind);
        field("smtp", "field.smtp", std::to_string(setup_.config.smtp_port));
        field("smtps", "field.smtps", std::to_string(setup_.config.smtps_port));
        field("imap", "field.imap", std::to_string(setup_.config.imap_port));
        field("pop3", "field.pop3", std::to_string(setup_.config.pop3_port));
        field("https", "field.https", std::to_string(setup_.config.https_port));
        field("submission", "field.submission", std::to_string(setup_.config.submission_port));
#ifdef _WIN32
        field("http_sys", "field.http_sys", setup_.config.webmail_driver == "http_sys" ? "1" : "0", false,
              true);
        view_.body += "\n\n" + tr("setup.native_note");
#else
        field("apache", "field.apache", setup_.config.webmail_driver == "apache" ? "1" : "0", false, true);
        view_.body += "\n\n" + tr("setup.apache_note");
#endif
        action("domain", "action.continue");
        if (setup_.step() == SetupStep::Ready)
            action("done", "action.done");
        break;
    case SetupStep::Dns:
        view_.title = tr("setup.dns_title");
        view_.body = tr("setup.dns_body") + "\n\n" + setup_.dns_records();
        field("token", "field.token", "", true);
        action("cloudflare", "action.cloudflare");
        action("manual", "action.dns_manual");
        action("back", "action.back");
        break;
    case SetupStep::Certificate:
        view_.title = tr("setup.cert_title");
        view_.body = tr("setup.cert_body") + "\n\n" + setup_.config.mail_host() + " · " +
                     setup_.config.webmail_host() + "\nhttps://letsencrypt.org/repository/";
        field("contact", "field.contact", setup_.contact());
        field("token", "field.token", "", true);
        field("tos", "field.tos", "", false, true);
        field("renew_force", "field.renew_force", "0", false, true);
        field("cert", "field.cert", path_utf8(setup_.config.certificate), false, false, true);
        field("key", "field.key", path_utf8(setup_.config.private_key), false, false, true);
        action("issue_cf", "action.issue_cf");
        action("issue_manual", "action.issue_manual");
        action("import", "action.import");
        action("back", "action.back");
        break;
    case SetupStep::Account:
    case SetupStep::Accounts:
        view_.title = tr(step == SetupStep::Account ? "setup.account_title" : "nav.accounts");
        view_.body = tr(step == SetupStep::Account ? "setup.account_body" : "settings.accounts_body");
        field("email", "field.email", step == SetupStep::Account ? "admin@" + setup_.config.domain : "");
        if (step == SetupStep::Accounts)
            field("old_password", "field.old_password", "", true);
        field("password", "field.password", "", true);
        field("confirmation", "field.confirmation", "", true);
        action(step == SetupStep::Account ? "account" : "account_add", "action.create_account");
        if (step == SetupStep::Accounts) {
            action("password_change", "action.change_password");
            action("done", "action.done");
        } else
            action("back", "action.back");
        break;
    case SetupStep::Preferences: {
        view_.title = tr("nav.preferences");
        view_.body = tr("settings.body");
        field("language", "field.language", setup_.config.language);
        view_.fields.back().choices = {{"pt-BR", "Português (Brasil)"},
                                       {"en", "English"},
                                       {"ja", "日本語"},
                                       {"ko", "한국어"},
                                       {"zh-TW", "繁體中文"}};
        field("connections", "field.connections", std::to_string(setup_.config.max_connections));
        field("message_limit", "field.message_limit",
              std::to_string(setup_.config.max_message_bytes / 1048576));
        field("quota", "field.quota", std::to_string(setup_.config.mailbox_quota_bytes / 1048576));
        field("timeout", "field.timeout", std::to_string(setup_.config.session_timeout_seconds));
#ifndef _WIN32
        field("backend", "field.backend", std::to_string(setup_.config.webmail_backend_port));
#endif
        field("outbound", "field.outbound", setup_.config.outbound_enabled ? "1" : "0", false, true);
        field("outbound_ipv6", "field.outbound_ipv6", setup_.config.outbound_ipv6 ? "1" : "0", false, true);
        field("antispam", "field.antispam", setup_.config.antispam_enabled ? "1" : "0", false, true);
        field("antispam_dns", "field.antispam_dns", setup_.config.antispam_dns ? "1" : "0", false, true);
        field("spam_threshold", "field.spam_threshold", std::to_string(setup_.config.antispam_threshold));
        std::string blocked;
        for (const auto &entry : setup_.config.antispam_blocklist) {
            if (!blocked.empty())
                blocked += ' ';
            blocked += entry;
        }
        field("spam_blocklist", "field.spam_blocklist", blocked);
        action("preferences_save", "action.save");
        action("done", "action.done");
        break;
    }
    case SetupStep::Ready:
        view_.title = tr("setup.ready_title");
        view_.body = tr("setup.ready_body");
        action("back", "nav.network");
        action("dns", "nav.dns");
        action("certificate", "nav.certificates");
        action("accounts", "nav.accounts");
        action("preferences", "nav.preferences");
        break;
    }
}
void Wizard::perform(const std::string &action, std::map<std::string, std::string> values) {
    {
        std::lock_guard lock(mutex_);
        if (view_.busy)
            return;
        if (action == "back" || action == "certificate" || action == "dns" || action == "preferences" ||
            action == "accounts" || action == "done") {
            auto step = action == "back"          ? SetupStep::Domain
                        : action == "certificate" ? SetupStep::Certificate
                        : action == "dns"         ? SetupStep::Dns
                        : action == "preferences" ? SetupStep::Preferences
                        : action == "accounts"    ? SetupStep::Accounts
                                                  : setup_.step();
            form(step);
            view_.log.clear();
            return;
        }
        view_.busy = true;
        view_.manual = action == "issue_manual";
        view_.log = tr("status.prepare") + "\n";
    }
    if (worker_.joinable())
        worker_.join();
    cancel_ = false;
    input_ = 0;
    worker_ = std::jthread([this, action, values = std::move(values)]() mutable {
        std::string error;
        auto output = [this](const std::string &text) {
            std::lock_guard lock(mutex_);
            view_.log += text;
            if (view_.log.size() > 65536)
                view_.log.erase(0, view_.log.size() - 65536);
        };
        auto number = [&](const char *key) -> unsigned long long {
            const auto &text = values.at(key);
            if (text.empty() || text.find_first_not_of("0123456789") != text.npos || text.size() > 12)
                throw std::runtime_error(tr("error.port"));
            return std::stoull(text);
        };
        try {
            if (action == "domain") {
                std::vector<unsigned short> ports;
                for (auto key : {"smtp", "smtps", "imap", "pop3", "https", "submission"}) {
                    auto value = values.contains(key) ? number(key) : setup_.config.submission_port;
                    if (!value || value > 65535)
                        throw std::runtime_error(tr("error.port"));
                    ports.push_back(static_cast<unsigned short>(value));
                }
#ifdef _WIN32
                auto driver = values.contains("http_sys")
                                  ? (values.at("http_sys") == "1" ? "http_sys" : "socket")
                                  : setup_.config.webmail_driver;
#else
                auto driver = values.contains("apache") ? (values.at("apache") == "1" ? "apache" : "socket")
                                                        : setup_.config.webmail_driver;
                if (std::string(driver) == "apache")
                    ports[4] = 443;
#endif
                setup_.configure(values.at("domain"), values.at("ipv4"), values.at("bind"), ports, driver);
                setup_.configure_webhost(output, cancel_, input_);
            } else if (action == "cloudflare")
                setup_.dns_cloudflare(values.at("token"));
            else if (action == "manual")
                setup_.dns_manual_confirmed();
            else if (action == "import") {
                setup_.import_certificate(utf8_path(values.at("cert")), utf8_path(values.at("key")));
                setup_.configure_webhost(output, cancel_, input_);
            } else if (action == "account")
                setup_.first_account(values.at("email"), values.at("password"), values.at("confirmation"));
            else if (action == "account_add" || action == "password_change") {
                auto email = email_address(values.at("email"));
                if (!email.ends_with("@" + setup_.config.domain))
                    throw std::runtime_error("Account must belong to the configured domain");
                if (values.at("password") != values.at("confirmation"))
                    throw std::runtime_error(tr("error.password_mismatch"));
                Database db(setup_.config);
                if (action == "account_add")
                    db.add_account(create_identity(email, values.at("password")).document);
                else {
                    auto doc = db.account(email);
                    if (!doc)
                        throw std::runtime_error("Account not found");
                    auto replacement =
                        change_password(*doc, values.at("old_password"), values.at("password"));
                    db.replace_account(email, *doc, replacement);
                }
            } else if (action == "preferences_save") {
                auto candidate = setup_.config;
                candidate.language = values.at("language");
                candidate.max_connections = number("connections");
                candidate.max_message_bytes = number("message_limit") * 1048576;
                candidate.mailbox_quota_bytes = number("quota") * 1048576;
                auto timeout = number("timeout");
                if (timeout > 3600)
                    throw std::runtime_error("Invalid session limit");
                candidate.session_timeout_seconds = static_cast<unsigned>(timeout);
                if (values.contains("backend")) {
                    auto port = number("backend");
                    if (port < 1024 || port > 65535)
                        throw std::runtime_error(tr("error.port"));
                    candidate.webmail_backend_port = static_cast<unsigned short>(port);
                }
                candidate.outbound_enabled = values.at("outbound") == "1";
                if (values.contains("outbound_ipv6"))
                    candidate.outbound_ipv6 = values.at("outbound_ipv6") == "1";
                if (values.contains("antispam"))
                    candidate.antispam_enabled = values.at("antispam") == "1";
                if (values.contains("antispam_dns"))
                    candidate.antispam_dns = values.at("antispam_dns") == "1";
                if (values.contains("spam_threshold")) {
                    auto threshold = number("spam_threshold");
                    if (threshold > 100)
                        throw std::runtime_error("Invalid spam threshold");
                    candidate.antispam_threshold = static_cast<int>(threshold);
                }
                if (values.contains("spam_blocklist")) {
                    candidate.antispam_blocklist.clear();
                    std::istringstream input(values.at("spam_blocklist"));
                    std::string entry;
                    while (input >> entry)
                        candidate.antispam_blocklist.push_back(entry);
                }
                candidate.validate();
                candidate.save(setup_.path());
                setup_.config = candidate;
                set_language(candidate.language);
                setup_.configure_webhost(output, cancel_, input_);
            } else if (action == "issue_cf" || action == "issue_manual") {
                if (values.at("tos") != "1")
                    throw std::runtime_error(tr("error.terms"));
                setup_.issue_certificate(values.at("contact"), action == "issue_cf" ? "cloudflare" : "manual",
                                         values.at("token"), output, cancel_, input_,
                                         values.contains("renew_force") && values.at("renew_force") == "1");
                setup_.configure_webhost(output, cancel_, input_);
            } else
                throw std::runtime_error("Unknown action");
        } catch (const std::exception &e) {
            error = e.what();
        }
        for (auto key : {"token", "password", "confirmation", "old_password"})
            if (values.contains(key))
                wipe(values.at(key));
        std::lock_guard lock(mutex_);
        if (error.empty()) {
            try {
                form(setup_.step());
                view_.log += "\n" + tr("status.saved");
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        if (!error.empty())
            view_.log += "\n" + tr("error.operation") + "\n" + error;
        view_.busy = false;
        view_.manual = false;
    });
}
} // namespace cm
