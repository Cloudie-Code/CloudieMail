#include "cloudiemail/dns.hpp"
#include "cloudiemail/i18n.hpp"
#include "cloudiemail/server.hpp"
#include "cloudiemail/setup.hpp"
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) {
    interrupted = 1;
}
std::string password(const char *prompt) {
    std::cerr << prompt << std::flush;
    std::string result;
#ifdef _WIN32
    if (_isatty(_fileno(stdin))) {
        for (;;) {
            int c = _getch();
            if (c == '\r')
                break;
            if (c == '\b') {
                if (!result.empty())
                    result.pop_back();
                continue;
            }
            if (c >= 32 && result.size() < 1024)
                result += static_cast<char>(c);
        }
        std::cerr << '\n';
    } else
        std::getline(std::cin, result);
#else
    termios old{};
    bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (terminal) {
        auto hidden = old;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    std::getline(std::cin, result);
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::cerr << '\n';
    }
#endif
    if (!std::cin)
        throw std::runtime_error("Nao foi possivel ler senha");
    return result;
}
void usage() {
    std::cout
        << "CloudieMail " CLOUDIEMAIL_VERSION "\n"
           "  cloudiemail setup [CONFIG]\n  cloudiemail configure CONFIG\n  cloudiemail init CONFIG\n  "
           "cloudiemail account-add CONFIG EMAIL\n"
           "  cloudiemail password-change CONFIG EMAIL\n  cloudiemail serve CONFIG\n  cloudiemail status "
           "CONFIG\n"
           "  cloudiemail dns-plan DOMAIN IPV4 [--json]\n  cloudiemail dns-apply DOMAIN IPV4 ZONE_ID\n\n"
        << cm::tr("console.help") << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
#ifndef _WIN32
        umask(0077);
        std::signal(SIGPIPE, SIG_IGN);
#endif
        cm::crypto_init();
        if (const char *language = std::getenv("CLOUDIEMAIL_LANGUAGE"))
            cm::set_language(language);
        if (argc < 2 || std::string(argv[1]) == "--help") {
            usage();
            return 0;
        }
        std::string command = argv[1];
        if (command == "setup" && argc <= 3) {
            cm::setup_console(argc == 3 ? std::filesystem::path(argv[2])
                                        : std::filesystem::path("config/local.json"));
            return 0;
        }
        if (command == "configure" && argc == 3) {
            cm::setup_console(argv[2], true);
            return 0;
        }
        if (command == "dns-plan" && argc >= 4) {
            auto plan = cm::dns_plan(argv[2], argv[3]);
            std::cout << (argc == 5 && std::string(argv[4]) == "--json" ? plan.dump(2)
                                                                        : cm::dns_zone_text(plan))
                      << '\n';
            return 0;
        }
        if (command == "dns-apply" && argc == 5) {
            auto token = std::getenv("CLOUDIEMAIL_CF_TOKEN");
            if (!token)
                throw std::runtime_error("Defina CLOUDIEMAIL_CF_TOKEN");
            std::cout << cm::apply_cloudflare(argv[4], token, cm::dns_plan(argv[2], argv[3])).dump(2) << '\n';
            return 0;
        }
        if (argc < 3) {
            usage();
            return 2;
        }
        auto config = cm::Config::load(argv[2]);
        cm::set_language(config.language);
        if (command == "init") {
            cm::Database database(config, true);
            std::cout << cm::tr("console.created_db") + ": " << config.database << '\n';
            return 0;
        }
        if (command == "serve") {
            unsigned run_for = 0;
            if (argc != 3) {
                if (argc != 5 || std::string(argv[3]) != "--run-for")
                    throw std::runtime_error("Use serve CONFIG [--run-for SEGUNDOS]");
                std::string value = argv[4];
                if (value.empty() || value.find_first_not_of("0123456789") != value.npos)
                    throw std::runtime_error("Duracao invalida");
                auto duration = std::stoul(value);
                if (duration < 1 || duration > 3600)
                    throw std::runtime_error("Duracao deve ser entre 1 e 3600 segundos");
                run_for = static_cast<unsigned>(duration);
            }
            cm::Server server(config);
            std::signal(SIGINT, on_signal);
            std::signal(SIGTERM, on_signal);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(run_for);
            std::jthread watcher([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    if (interrupted || (run_for && std::chrono::steady_clock::now() >= deadline)) {
                        server.stop();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            server.run();
            watcher.request_stop();
            return 0;
        }
        cm::Database database(config);
        if (command == "status") {
            std::cout << database.statistics().dump(2) << '\n';
            return 0;
        }
        if (command == "account-add" && argc == 4) {
            auto email = cm::email_address(argv[3]);
            if (!email.ends_with("@" + config.domain))
                throw std::runtime_error("Email fora do dominio configurado");
            auto pass = password((cm::tr("field.password") + ": ").c_str());
            auto confirm = password((cm::tr("field.confirmation") + ": ").c_str());
            if (pass != confirm) {
                cm::wipe(pass);
                cm::wipe(confirm);
                throw std::runtime_error(cm::tr("error.password_mismatch"));
            }
            cm::wipe(confirm);
            auto identity = cm::create_identity(email, pass);
            cm::wipe(pass);
            database.add_account(identity.document);
            std::cout << cm::tr("console.created_account") + ": " << email
                      << "\n" + cm::tr("console.key_id") + ": "
                      << identity.document.at("key_id").get<std::string>() << '\n';
            return 0;
        }
        if (command == "password-change" && argc == 4) {
            auto email = cm::email_address(argv[3]);
            auto doc = database.account(email);
            if (!doc)
                throw std::runtime_error("Conta inexistente");
            auto old = password((cm::tr("field.old_password") + ": ").c_str()),
                 next = password((cm::tr("field.password") + ": ").c_str()),
                 confirm = password((cm::tr("field.confirmation") + ": ").c_str());
            if (next != confirm)
                throw std::runtime_error(cm::tr("error.password_mismatch"));
            cm::wipe(confirm);
            auto replacement = cm::change_password(*doc, old, next);
            cm::wipe(old);
            cm::wipe(next);
            database.replace_account(email, *doc, replacement);
            std::cout << cm::tr("console.password_changed") << "\n";
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception &error) {
        std::cerr << cm::tr("error.operation") << " " << error.what() << '\n';
        return 1;
    }
}
