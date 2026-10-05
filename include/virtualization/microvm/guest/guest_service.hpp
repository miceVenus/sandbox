#pragma once

#include "../virtualization/microvm/communication/agent_transport.hpp"

namespace protocol {
    struct GuestConfig {
        std::filesystem::path workspace;
        Limits limits;
        std::chrono::milliseconds io_timeout{5000};
        std::chrono::milliseconds idle_timeout{300000};
        // Set only by the privileged Guest bootstrap, never by protocol requests.
        std::filesystem::path task_cgroup;
        std::filesystem::path task_launcher;
        Json runtime_info = Json::object();
    };

    // Internal Guest component, not a host isolation backend. The caller has already
    // established the VM, identity/resource boundaries and private transport.
    // On EOF or protocol failure, active tasks are cancelled and writes abandoned.
    void serve_guest(std::unique_ptr<Transport> transport, const GuestConfig &config);
} // namespace protocol
