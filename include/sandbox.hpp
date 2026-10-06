#pragma once
#include "sandbox_types.hpp"
#include "virtualization/runtime.hpp"
#include "workspace/workspace.hpp"
#include <memory>
#include <optional>

auto default_sandbox_root() -> std::filesystem::path;

// One handle represents one workspace and one runtime. Reopen persisted resources
// explicitly; destruction of this C++ handle does not discard the workspace.
// Release the old handle before opening the same running environment again:
// agentd accepts one owned connection at a time.
class Sandbox {
  public:
    explicit Sandbox(const std::filesystem::path& root = default_sandbox_root());
    Sandbox(const std::filesystem::path& root,
            std::unique_ptr<RuntimeBackend> runtime);
    Sandbox(const Sandbox &) = delete;
    auto operator=(const Sandbox &) -> Sandbox & = delete;

    auto create(const Options &options) -> SandboxInfo;
    void open(const std::string &id);
    [[nodiscard]] auto id() const -> const std::string &;
    auto info() -> SandboxInfo;
    auto execute(const CommandRequest &request, OutputCallback on_output = {}) -> Result;
    auto execute(const std::vector<std::string> &argv, OutputCallback on_output = {}) -> Result;
    auto cancel() -> bool;
    auto read(const std::filesystem::path &container_path) -> std::string;
    void write(const std::filesystem::path &container_path, std::string_view content);
    auto status() -> SandboxStatus;
    auto get_status() -> SandboxState;
    auto get_changes() -> Changes;
    void stop();
    // Idempotent discard after verified reclamation; this handle cannot be reused.
    void destroy();

  private:
    auto execute_locked(SandboxInfo &info, const CommandRequest &request, OutputCallback on_output) -> Result;
    void require_active(SandboxInfo &info);
    void handle_execution_result(SandboxInfo &info, const Result &result);
    auto load(const std::string &id) -> SandboxInfo;
    void save(const SandboxInfo &info);
    std::filesystem::path root_;
    std::unique_ptr<RuntimeBackend> runtime_;
    std::optional<SandboxInfo> info_;
    bool destroyed_ = false;
};
