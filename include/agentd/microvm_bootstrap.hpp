#pragma once
#include "agentd/service.hpp"

namespace agentd {
    void configure_microvm(ServiceConfig &config, const std::filesystem::path &settings);
} // namespace agentd
