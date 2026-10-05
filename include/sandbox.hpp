#pragma once
#include "sandbox_types.hpp"
#include "virtualization/runtime.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_backend.hpp"
#include <memory>
#include <optional>

std::filesystem::path default_sandbox_root();

// One handle represents one workspace and one runtime. Reopen persisted resources
// explicitly; destruction of this C++ handle does not discard the workspace.
// Release the old handle before opening the same running environment again:
// agentd accepts one owned connection at a time.
class Sandbox {
  public:
    explicit Sandbox(std::filesystem::path root = default_sandbox_root());
    Sandbox(std::filesystem::path root,
            std::unique_ptr<RuntimeBackend> runtime,
            std::unique_ptr<WorkspaceBackend> workspace = make_git_workspace_backend());
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    SandboxInfo create(const Options &options);
    void open(const std::string &id);
    const std::string &id() const;
    SandboxInfo info();
    Result execute(const CommandRequest &request, OutputCallback on_output = {});
    Result execute(const std::vector<std::string> &argv, OutputCallback on_output = {});
    bool cancel();
    std::string read(const std::filesystem::path &container_path);
    void write(const std::filesystem::path &container_path, std::string_view content);
    SandboxStatus status();
    SandboxState get_status();
    Changes get_changes();
    void stop();
    // Idempotent discard after verified reclamation; this handle cannot be reused.
    void destroy();

  private:
    Result execute_locked(SandboxInfo &info, const CommandRequest &request, OutputCallback on_output);
    void require_active(SandboxInfo &info);
    void handle_execution_result(SandboxInfo &info, const Result &result);
    SandboxInfo load(const std::string &id);
    void save(const SandboxInfo &info);
    std::filesystem::path root_;
    std::unique_ptr<RuntimeBackend> runtime_;
    std::unique_ptr<WorkspaceBackend> workspace_;
    std::optional<SandboxInfo> info_;
    bool destroyed_ = false;
};
