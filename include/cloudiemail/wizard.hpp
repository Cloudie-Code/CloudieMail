#pragma once
#include "setup.hpp"
#include <mutex>
#include <thread>
namespace cm {
struct WizardField {
    std::string id, label, value;
    bool secret = false, checkbox = false, file = false;
    std::vector<std::pair<std::string, std::string>> choices;
};
struct WizardAction {
    std::string id, label;
};
struct WizardView {
    SetupStep step = SetupStep::Domain;
    std::string title, body, log;
    std::vector<WizardField> fields;
    std::vector<WizardAction> actions;
    bool busy = false, manual = false;
    unsigned revision = 0;
};
class Wizard {
    Setup setup_;
    WizardView view_;
    std::mutex mutex_;
    std::jthread worker_;
    std::atomic_bool cancel_{false};
    std::atomic_uint input_{0};
    void form(SetupStep step);

  public:
    explicit Wizard(const std::filesystem::path &path);
    ~Wizard();
    WizardView view();
    void language(const std::string &code);
    void perform(const std::string &action, std::map<std::string, std::string> values);
    void cancel() {
        cancel_ = true;
    }
    void next_challenge() {
        ++input_;
    }
};
} // namespace cm
