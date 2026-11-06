#include "cloudiemail/i18n.hpp"
#include "cloudiemail/wizard.hpp"
#include <csignal>
#include <iostream>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
namespace cm {
namespace {
volatile std::sig_atomic_t setup_interrupted = 0;
void interrupt_setup(int) {
    setup_interrupted = 1;
}
std::string answer(const WizardField &field) {
    std::cout << field.label;
    if (!field.value.empty())
        std::cout << " ["
                  << (field.checkbox ? (field.value == "1" ? tr("console.yes") : tr("console.no"))
                                     : field.value)
                  << "]";
    if (field.checkbox)
        std::cout << " (" << tr("console.yes") << "/" << tr("console.no") << ")";
    if (!field.choices.empty())
        for (const auto &[code, name] : field.choices)
            std::cout << " " << code << "=" << name;
    std::cout << ": " << std::flush;
    std::string value;
#ifdef _WIN32
    if (field.secret && _isatty(_fileno(stdin))) {
        for (;;) {
            int c = _getch();
            if (c == '\r')
                break;
            if (c == '\b') {
                if (!value.empty())
                    value.pop_back();
            } else if (c >= 32 && value.size() < 1024)
                value += static_cast<char>(c);
        }
        std::cout << '\n';
    } else
        std::getline(std::cin, value);
#else
    termios old{};
    bool hidden = field.secret && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (hidden) {
        auto next = old;
        next.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &next);
    }
    std::getline(std::cin, value);
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::cout << '\n';
    }
#endif
    if (!std::cin)
        throw std::runtime_error(tr("console.eof"));
    if (field.checkbox)
        return value.empty() && !field.value.empty()
                   ? field.value
                   : (value == tr("console.yes") || value == "sim" || value == "yes" || value == "1" ? "1"
                                                                                                     : "0");
    return value.empty() ? field.value : value;
}
} // namespace
void setup_console(const std::filesystem::path &path, bool manage) {
    Wizard wizard(path);
    setup_interrupted = 0;
    auto previous = std::signal(SIGINT, interrupt_setup);
    struct Restore {
        decltype(previous) handler;
        ~Restore() {
            std::signal(SIGINT, handler);
        }
    } restore{previous};
    for (;;) {
        auto view = wizard.view();
        std::cout << "\n" << view.title << "\n" << view.body << "\n";
        if (view.step == SetupStep::Ready && !manage) {
            std::cout << tr("console.ready") << "\n";
            return;
        }
        if (manage)
            std::cout << "0) " << tr("action.exit") << "\n";
        for (std::size_t i = 0; i < view.actions.size(); ++i)
            std::cout << i + 1 << ") " << view.actions[i].label << '\n';
        auto selected = answer({"", tr("console.option"), "1", false, false, false, {}});
        std::size_t choice = 0;
        try {
            choice = std::stoul(selected);
        } catch (...) {
            continue;
        }
        if (manage && choice == 0)
            return;
        if (!choice || choice > view.actions.size())
            continue;
        auto action = view.actions[choice - 1].id;
        std::map<std::string, std::string> values;
        for (const auto &field : view.fields) {
            bool needed = action == "domain" || action == "account" || action == "preferences_save" ||
                          action == "account_add" || action == "password_change" ||
                          (action == "cloudflare" && field.id == "token") ||
                          (action == "import" && (field.id == "cert" || field.id == "key")) ||
                          ((action == "issue_cf" || action == "issue_manual") &&
                           (field.id == "contact" || field.id == "tos" || field.id == "token" ||
                            field.id == "renew_force"));
            values[field.id] = needed ? answer(field) : field.value;
        }
        wizard.perform(action, std::move(values));
        std::size_t printed = 0, prompts = 0;
        for (;;) {
            view = wizard.view();
            if (printed > view.log.size())
                printed = 0;
            std::cout << view.log.substr(printed) << std::flush;
            printed = view.log.size();
            if (setup_interrupted)
                wizard.cancel();
            auto prompt = view.log.find("Press 'Enter'", prompts);
            if (view.manual && prompt != view.log.npos) {
                std::cout << "\n" << tr("console.txt") << " " << std::flush;
                std::string line;
                std::getline(std::cin, line);
                if (!std::cin)
                    wizard.cancel();
                else
                    wizard.next_challenge();
                prompts = prompt + 1;
            }
            if (!view.busy)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (setup_interrupted)
            throw std::runtime_error(tr("console.cancelled"));
    }
}
} // namespace cm
