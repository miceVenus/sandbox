#pragma once
#include <filesystem>
#include <string_view>
#include <vector>

namespace runtime_files {
    // Parse trusted ldd output by line; library paths can contain whitespace.
    std::vector<std::filesystem::path> dynamic_dependency_paths(std::string_view output);
}

// Copy a trusted SDK/system ELF and its dynamic dependencies into a private root.
void install_runtime_program(const std::filesystem::path &root,
                             const std::filesystem::path &program,
                             const std::filesystem::path &destination);
