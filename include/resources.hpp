#pragma once
#include <filesystem>

namespace sandbox_resources {
    // CMake selects the development/install resource directory. A relocated static
    // SDK may explicitly set BBM_SANDBOX_RESOURCE_DIR in the trusted host service.
    auto crun_worker_path() -> std::filesystem::path;
    auto agentd_path() -> std::filesystem::path;
    auto krun_runner_path() -> std::filesystem::path;
    auto krun_firmware_path() -> std::filesystem::path;
} // namespace sandbox_resources
