#include "cloudiemail/server.hpp"
#include "cloudiemail/setup.hpp"
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
    ++checks;
}
template <class F> void rejects(F f, const char *message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
} // namespace
int main(int argc, char **argv) {
    try {
        cm::crypto_init();
        bool process_only = argc == 2 && std::string(argv[1]) == "--process-only";
        if (!process_only) {
            if (argc != 4)
                throw std::runtime_error("Expected directory, certificate and key");
            auto root = std::filesystem::absolute(argv[1]);
            cm::Setup setup(root / "config/local.json");
            check(setup.step() == cm::SetupStep::Domain, "Fresh installation skipped domain");
            rejects(
                [&] {
                    setup.first_account("admin@example.test", "secure password 123!", "secure password 123!");
                },
                "Account allowed before configuration");
            setup.configure("example.test", "192.0.2.10", "127.0.0.1", {2525, 2465, 2993, 2995, 8443});
            check(setup.step() == cm::SetupStep::Dns, "DNS stage missing");
            check(setup.dns_records().find("webmail.example.test") != std::string::npos,
                  "Webmail missing in DNS");
            rejects(
                [&] {
                    setup.first_account("admin@example.test", "secure password 123!", "secure password 123!");
                },
                "Account allowed before DNS/TLS");
            setup.dns_manual_confirmed();
            check(setup.step() == cm::SetupStep::Certificate, "TLS stage missing");
            rejects([&] { setup.import_certificate(argv[3], argv[2]); }, "Invalid certificate accepted");
            check(setup.step() == cm::SetupStep::Certificate, "Failed import advanced stage");
            setup.import_certificate(argv[2], argv[3]);
            check(setup.step() == cm::SetupStep::Account, "First account stage missing");
            check(setup.config.private_key != std::filesystem::path(argv[3]),
                  "Imported key not copied into managed storage");
            {
                cm::Database db(setup.config, true);
            }
            rejects([&] { cm::Server server(setup.config); }, "Server allowed zero accounts");
            rejects(
                [&] {
                    setup.first_account("admin@example.test", "secure password 123!", "different password!");
                },
                "Password mismatch accepted");
            rejects(
                [&] {
                    setup.first_account("admin@outside.test", "secure password 123!", "secure password 123!");
                },
                "Foreign domain accepted");
            rejects([&] { setup.first_account("admin@example.test", "short", "short"); },
                    "Weak password accepted");
            setup.first_account("admin@example.test", "secure password 123!", "secure password 123!");
            check(setup.step() == cm::SetupStep::Ready, "Wizard did not finish");
            cm::Setup resumed(root / "config/local.json");
            check(resumed.step() == cm::SetupStep::Ready, "Wizard did not resume");
            rejects(
                [&] {
                    setup.first_account("second@example.test", "secure password 123!",
                                        "secure password 123!");
                },
                "First-account operation repeated");
            rejects(
                [&] {
                    setup.configure("other.test", "192.0.2.10", "127.0.0.1", {2525, 2465, 2993, 2995, 8443});
                },
                "Domain of populated database changed");
            cm::Config wrong = setup.config;
            wrong.domain = "other.test";
            rejects([&] { cm::validate_certificate(wrong); }, "Wrong certificate hostname accepted");
            for (const auto &path :
                 {setup.path(), std::filesystem::path(setup.path().string() + ".setup.json")}) {
                std::ifstream file(path);
                std::string content((std::istreambuf_iterator<char>(file)), {});
                check(content.find("secure password") == content.npos, "Password persisted in configuration");
            }
        }
        auto probe = std::filesystem::absolute(argv[0]).parent_path() /
#ifdef _WIN32
                     "cloudiemail-process-probe.exe";
#else
                     "cloudiemail-process-probe";
#endif
        std::atomic_bool cancel{false};
        std::atomic_uint input{1};
        std::string output;
        auto result = cm::run_child(
            probe, {"--child", "argument with spaces and \"quotes\""}, {},
            [&](const std::string &chunk) { output += chunk; }, cancel, input);
        check(result == 0 && output.find("child done") != output.npos,
              "Child process pipe/argument handling failed");
        cancel = true;
        result = cm::run_child(probe, {"--wait"}, {}, [](const std::string &) {}, cancel, input);
        check(result != 0, "Child cancellation ignored");
        std::cout << checks << " onboarding/process checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
