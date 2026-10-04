#pragma once
#include <filesystem>

// Copy a trusted SDK/system ELF and its dynamic dependencies into a private root.
void install_runtime_program(const std::filesystem::path &root,
                             const std::filesystem::path &program,
                             const std::filesystem::path &destination);
