#pragma once
#include "client.hpp"
#include "workspace.hpp"
#include <chrono>
#include <optional>

enum class MergePolicy { ReviewOnly, AutoFastForward };
// Minimal copies a small runtime. HostTools imports selected host tool directories read-only.
enum class Environment { Minimal, HostTools };
enum class SessionState {
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
    std::filesystem::path src_repo, ctr_repo = "/workspace", cwd_rlt = ".";
    size_t memory_bytes = 256 * 1024 * 1024;
    size_t cpu_period_us = 100000, cpu_quota_us = 100000, max_tasks = 64;
    std::chrono::milliseconds cmd_timeout{2000};
    size_t max_output_bytes = 8 * 1024 * 1024;
    size_t max_file_bytes = 8 * 1024 * 1024;
    MergePolicy policy = MergePolicy::ReviewOnly;
    Environment environment = Environment::HostTools;
};
struct Session {
    std::string s_id, c_id;
    Options options;
    std::filesystem::path s_dir, bundle_dir, work_files_dir;
    std::filesystem::path helper_container_path = "/sandbox-tools/sandbox-io";
    std::string base_commit, source_head_at_creation;
    std::optional<std::string> target_branch, result_commit;
    SessionState state = SessionState::Preparing;
    std::string last_error;
    bool rootless = false;
    bool resource_limits_verified = false;
};
struct CommandRequest {
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> cwd_relative;
    // Binary bytes; not shell text. Empty input gives the command immediate EOF.
    std::string stdin_data;
    // Environment is fixed by the OCI configuration, including isolated Git settings.
};

struct SessionStatus {
    Session session;
    std::string container_status;
    std::string runtime_error;
};

std::filesystem::path default_manager_root();

class SandboxManager {
  public:
    explicit SandboxManager(std::filesystem::path root = default_manager_root(),
                            std::string runtime_binary = "/usr/local/bin/crun");

    Session create_session(const Options &options);
    Result execute(const std::string &id, const CommandRequest &request);
    Result execute(const std::string &id, const std::vector<std::string> &argv);
    // Absolute container paths within ctr_repo. Parent directories must exist.
    // Requires an Active session; throws on errors, with no partial read result.
    std::string read(const std::string &id, const std::filesystem::path &container_path);
    void write(const std::string &id,
               const std::filesystem::path &container_path,
               std::string_view content);
    SessionStatus get_session_status(const std::string &id);
    SessionState get_status(const std::string &id);
    Changes get_changes(const std::string &id);
    void stop_session(const std::string &id);

  private:
    Result execute_locked(Session &session, const CommandRequest &request);
    Session load(const std::string &id);
    void save(const Session &session);
    std::filesystem::path root_;
    CrunClient runtime_;
};
int session_cli(int argc, char **argv);
