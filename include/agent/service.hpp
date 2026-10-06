#pragma once

#include "ipc/transport.hpp"
#include <functional>

namespace protocol {
    struct AgentConfig {
        std::filesystem::path workspace;
        Limits limits;
        std::chrono::milliseconds io_timeout{5000};
        // Zero keeps an owned persistent connection open until EOF.
        std::chrono::milliseconds idle_timeout{0};
        // Set only by the trusted bootstrap, never by protocol requests.
        std::filesystem::path task_launcher;
        bool mapped_file_identity = false;
        std::vector<std::string> task_environment;
        // Bootstrap-owned policies; requests cannot choose cleanup or freeze mechanisms.
        std::function<void()> finish_tasks;
        std::function<void(bool)> freeze_workspace;
        Json runtime_info = Json::object();
    };

    // Internal execution-environment component, not a host isolation backend. The caller has already
    // established the isolation, identity/resource boundaries and private transport.
    // On EOF or protocol failure, active tasks are cancelled and writes abandoned.
    void serve_agent(std::unique_ptr<Transport> transport, const AgentConfig &config);
} // namespace protocol
