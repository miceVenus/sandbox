#pragma once

#include "process.hpp"
#include <filesystem>
#include <memory>
#include <string>

struct SandboxInfo;
struct Options;

enum class RuntimeState { Missing, Running, Paused, Stopped, Unknown };

struct RuntimeStatus {
    RuntimeState state = RuntimeState::Unknown;
    // A failed query differs from a positively confirmed Missing runtime.
    bool verified = false;
    std::string detail;
    std::string error;
};

enum class OutputStream { Stdout, Stderr };
// Called synchronously on execute's caller thread with binary chunks. The view is
// valid only during the callback; calling another blocking Sandbox API is rejected.
using OutputCallback = std::function<void(OutputStream, std::string_view)>;

struct RuntimeCommand {
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    std::string stdin_data;
    int timeout_ms = 2000;
    size_t output_limit = 8 * 1024 * 1024;
    OutputCallback on_output{};
};

// Host/guest transport, artifact format and isolation belong to the backend.
// A VM backend can import B into a disk image and use a guest agent for file/exec RPC.
class RuntimeBackend {
  public:
    virtual ~RuntimeBackend() = default;
    virtual std::string id() const = 0;
    virtual void configure_state_directory(const std::filesystem::path &directory) = 0;
    virtual void validate_options(const Options &options) = 0;
    virtual void prepare(SandboxInfo &info) = 0;
    // Return only after startup and resource policy enforcement are verified.
    virtual void start(SandboxInfo &info) = 0;
    virtual RuntimeStatus status(const SandboxInfo &info) = 0;
    virtual Result execute(const SandboxInfo &info, const RuntimeCommand &command) = 0;
    // May be invoked concurrently with execute; false means no active task.
    virtual bool cancel() { return false; }
    virtual Result
    read(const SandboxInfo &info, const std::filesystem::path &path, size_t limit) = 0;
    virtual Result
    write(const SandboxInfo &info, const std::filesystem::path &path, std::string_view content) = 0;
    virtual void pause(const SandboxInfo &info) = 0;
    virtual void resume(const SandboxInfo &info) = 0;
    // Called with writers quiesced. Export guest changes to B before host-side inspection.
    virtual void synchronize_workspace(const SandboxInfo &info) = 0;
    // Idempotent; reclaim all execution resources, retaining B and info artifacts.
    virtual void stop(const SandboxInfo &info) = 0;
};

std::unique_ptr<RuntimeBackend> make_libcrun_backend();
