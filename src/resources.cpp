#include "resources.hpp"
#include "resources_config.hpp"
#include <cstdlib>
#include <stdexcept>

namespace sandbox_resources {
    namespace {
        auto directory() -> std::filesystem::path {
            const auto *override = std::getenv("BBM_SANDBOX_RESOURCE_DIR");
            std::filesystem::path path(override ? override : configured_resource_directory);
            if (!path.is_absolute())
                throw std::runtime_error("SDK resource directory must be absolute");
            return path;
        }
    } // namespace
    auto crun_worker_path() -> std::filesystem::path {
        return directory() / "sandbox-crun";
    }
    auto agentd_path() -> std::filesystem::path {
        return directory() / "agentd";
    }
    auto krun_runner_path() -> std::filesystem::path {
        return directory() / "sandbox-krun";
    }
    auto krun_firmware_path() -> std::filesystem::path {
        return directory() / "lib/libkrunfw.so.5";
    }
} // namespace sandbox_resources
