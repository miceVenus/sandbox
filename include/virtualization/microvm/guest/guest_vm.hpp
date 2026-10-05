#pragma once
#include "agent/service.hpp"

namespace protocol {
    void configure_guest_vm(AgentConfig &config, const std::filesystem::path &settings);
}
