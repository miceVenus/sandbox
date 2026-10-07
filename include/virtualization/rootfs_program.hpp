#pragma once
#include <filesystem>

// Install a trusted executable and its dependencies into a private sandbox rootfs.
void install_runtime_program(const std::filesystem::path &root,
                             const std::filesystem::path &program,
                             const std::filesystem::path &destination);
