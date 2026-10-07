#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

enum class MergePolicy : uint8_t { ReviewOnly, AutoFastForward };

// Minimal copies a small runtime. HostTools imports selected host tool directories read-only.
enum class Environment : uint8_t { Minimal, HostTools };

enum class SandboxState : uint8_t {
    Preparing,
    Active,
    Frozen,
    Committed,
    Applied,
    Failed,
    Discarded,
    Stopped
};

struct Options {
    std::string revision = "HEAD";
    std::filesystem::path src_repo;
    std::filesystem::path ctr_repo = "/workspace";
    std::filesystem::path cwd_rlt = ".";
    size_t memory_bytes = 256 * 1024 * 1024;
    size_t cpu_period_us = 100000;
    size_t cpu_quota_us = 100000;
    size_t max_tasks = 64;
    std::chrono::milliseconds cmd_timeout{2000};
    size_t max_output_bytes = 8 * 1024 * 1024;
    size_t max_file_bytes = 8 * 1024 * 1024;
    MergePolicy policy = MergePolicy::ReviewOnly;
    Environment environment = Environment::HostTools;
};
struct SandboxInfo {
    std::string id;
    std::string runtime_id;
    Options options;
    std::filesystem::path directory;
    // Compatibility artifact path; only OCI backends prepare a bundle here.
    std::filesystem::path bundle_dir;
    std::filesystem::path work_files_dir;
    std::string base_commit;
    std::string source_head_at_creation;
    std::optional<std::string> target_branch;
    std::optional<std::string> result_commit;
    SandboxState state = SandboxState::Preparing;
    std::string last_error;
    std::string runtime_backend = "oci-crun";
    bool rootless = false;
    bool resource_limits_verified = false;
};
struct CommandRequest {
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> cwd_relative;
    // Binary bytes; not shell text. Empty input gives the command immediate EOF.
    std::string stdin_data;
    // Task environment is enforced by the selected runtime backend.
};

struct SandboxStatus {
    SandboxInfo info;
    // Verified execution status reported by the selected backend.
    std::string runtime_status;
    std::string runtime_error;
};
