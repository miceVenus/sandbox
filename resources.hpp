#pragma once

#include <filesystem>

namespace sandbox_resources {
    inline constexpr const char *container_file_helper = "/sandbox-tools/sandbox-io";

    // Set by CMake for the SDK; independent of the caller's executable location.
    std::filesystem::path file_helper_path();
} // namespace sandbox_resources
