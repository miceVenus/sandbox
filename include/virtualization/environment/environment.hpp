#pragma once

#include <array>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct Options;
struct SandboxInfo;

namespace virtualization::environment {
    inline constexpr std::array<const char *, 3> runtime_directories = {"build", "env", "cache"};

    struct ToolMount {
        std::filesystem::path source;
        std::filesystem::path destination;
    };

    // Computed once and shared by rootfs preparation, OCI mounts and agentd settings.
    struct Config {
        std::vector<ToolMount> tool_mounts;
        std::vector<std::string> variables;
    };

    auto make_config(const Options &options) -> Config;
    void validate_host_tools(const Config &config, const std::filesystem::path &source_repository,
                             const std::filesystem::path &manager_root);
    void prepare_host_tools(const std::filesystem::path &rootfs, const Config &config);
    void prepare_runtime_data(const SandboxInfo &info);
    void prepare_rootfs(const SandboxInfo &info, const Config &config);
    void install_program(const std::filesystem::path &rootfs,
                         const std::filesystem::path &executable,
                         const std::filesystem::path &destination);
    // Build common OCI settings; the backend supplies process.args before writing the file.
    auto make_oci_config(const SandboxInfo &info, const Config &config) -> nlohmann::json;
} // namespace virtualization::environment
