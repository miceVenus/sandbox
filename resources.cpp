#include "resources.hpp"
#include "resources_config.hpp"

namespace sandbox_resources {
    std::filesystem::path file_helper_path() {
        return configured_file_helper;
    }
} // namespace sandbox_resources
