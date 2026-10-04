#include "../include/resources.hpp"
#include "resources_config.hpp"

namespace sandbox_resources {
    std::filesystem::path file_helper_path() {
        return configured_file_helper;
    }
    std::filesystem::path runtime_runner_path() {
        return configured_runtime_runner;
    }
    std::filesystem::path git_worker_path() {
        return configured_git_worker;
    }
    std::filesystem::path guest_agent_path() {
        return configured_guest_agent;
    }
    std::filesystem::path krun_runner_path() {
        return configured_krun_runner;
    }
    std::filesystem::path krun_firmware_path() {
        return configured_krun_firmware;
    }
} // namespace sandbox_resources
