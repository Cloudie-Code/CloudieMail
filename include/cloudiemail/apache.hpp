#pragma once
#include "setup.hpp"
namespace cm {
void configure_apache(const std::filesystem::path &config, bool disable, const Progress &output,
                      std::atomic_bool &cancel, std::atomic_uint &input);
}
