#pragma once
#include "agentd/service.hpp"

namespace agentd {
    // Only the container's PID 1 may enter this mode; the listener is inherited
    // from the host and its filesystem endpoint is never mounted into the task.
    void configure_container(ServiceConfig &config, const std::filesystem::path &settings);
} // namespace agentd
