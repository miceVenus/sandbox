#include "resources.hpp"
#include "resources_config.hpp"
#include <cstdlib>
#include <stdexcept>

namespace sandbox_resources {
    namespace {
        std::filesystem::path directory() {
            const auto *override = std::getenv("BBM_SANDBOX_RESOURCE_DIR");
            std::filesystem::path path(override ? override : configured_resource_directory);
            if (!path.is_absolute()) throw std::runtime_error("SDK resource directory must be absolute");
            return path;
        }
    }
    std::filesystem::path runtime_runner_path() { return directory() / "sandbox-crun"; }
    std::filesystem::path guest_agent_path() { return directory() / "agentd"; }
    std::filesystem::path krun_runner_path() { return directory() / "sandbox-krun"; }
    std::filesystem::path krun_firmware_path() { return directory() / "lib/libkrunfw.so.5"; }
}
