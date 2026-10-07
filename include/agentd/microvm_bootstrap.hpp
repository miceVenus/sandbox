#pragma once
#include "agentd/service.hpp"

namespace agentd {
    void configure_microvm(AgentdConfig &config, const std::filesystem::path &settings);
}
