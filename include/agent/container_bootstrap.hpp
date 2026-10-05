#pragma once
#include "service.hpp"

namespace protocol {
    // Only the container's PID 1 may enter this mode; the listener is inherited
    // from the host and its filesystem endpoint is never mounted into the task.
    void configure_container_agent(AgentConfig &config, const std::filesystem::path &settings);
}
