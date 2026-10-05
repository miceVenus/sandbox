#pragma once
#include <filesystem>

namespace sandbox_resources {
    // CMake selects the development/install resource directory. A relocated static
    // SDK may explicitly set BBM_SANDBOX_RESOURCE_DIR in the trusted host service.
    std::filesystem::path runtime_runner_path();
    std::filesystem::path guest_agent_path();
    std::filesystem::path krun_runner_path();
    std::filesystem::path krun_firmware_path();
}
