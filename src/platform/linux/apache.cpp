#include "cloudiemail/apache.hpp"
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif
namespace cm {
void configure_apache(const std::filesystem::path &config, bool disable, const Progress &output,
                      std::atomic_bool &cancel, std::atomic_uint &input) {
#ifdef _WIN32
    (void)config;
    (void)disable;
    (void)output;
    (void)cancel;
    (void)input;
    throw std::runtime_error("Apache integration is available on Linux only");
#else
    auto helper = executable_directory().parent_path() / "scripts/configure-apache.py";
    if (!std::filesystem::is_regular_file(helper))
        helper = executable_directory() / "scripts/configure-apache.py";
    if (!std::filesystem::is_regular_file(helper))
        throw std::runtime_error("Apache configuration helper is missing");
    std::vector<std::string> args;
    std::filesystem::path executable = "/usr/bin/python3";
    if (geteuid() != 0) {
        executable = "/usr/bin/pkexec";
        args.push_back("/usr/bin/python3");
    }
    args.push_back(path_utf8(helper));
    args.push_back(path_utf8(std::filesystem::absolute(config)));
    if (disable)
        args.push_back("--disable");
    output("Apache: requesting administrator authentication...\n");
    if (run_child(executable, args, {}, output, cancel, input) != 0)
        throw std::runtime_error("Apache configuration failed; see the activity log");
#endif
}
} // namespace cm
