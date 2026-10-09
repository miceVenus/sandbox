#pragma once

#include "ipc/protocol.hpp"
#include "lib/descriptor.hpp"
#include <chrono>
#include <filesystem>
#include <functional>

namespace agentd {
    struct ServiceConfig {
        std::filesystem::path workspace;
        ipc::Limits limits;
        std::chrono::milliseconds io_timeout{5000};
        // Zero keeps an owned persistent connection open until EOF.
        std::chrono::milliseconds idle_timeout{0};
        // Set only by the trusted bootstrap, never by protocol requests.
        std::filesystem::path task_launcher;
        bool mapped_file_identity = false;
        std::vector<std::string> task_environment;
        // Task cleanup and workspace freeze are configured by the bootstrap.
        std::function<void()> cleanup_tasks;
        std::function<void(bool)> freeze_workspace;
        ipc::Json runtime_info = ipc::Json::object();
    };

    // Internal execution-environment component, not a host isolation backend. The caller has
    // already established the isolation, identity/resource boundaries and private transport. On EOF
    // or protocol failure, active tasks are cancelled and writes abandoned.
    void serve(lib::UniqueFd descriptor, const ServiceConfig &config);
} // namespace agentd
