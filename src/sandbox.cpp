#include "sandbox.hpp"
#include "lib.hpp"
#include "sandbox_types.hpp"
#include "workspace/workspace.hpp"
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

    struct SandboxLock {
        int file_descriptor;
        explicit SandboxLock(const fs::path &lock_path)
            : file_descriptor(open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
            if (file_descriptor < 0) {
                throw std::runtime_error("cannot open sandbox lock");
            }
            if (flock(file_descriptor, LOCK_EX | LOCK_NB)) {
                close(file_descriptor);
                throw std::runtime_error("sandbox busy");
            }
        }
        ~SandboxLock() {
            close(file_descriptor);
        }
        SandboxLock(const SandboxLock &) = delete;
    };

    auto encode_sandbox_info(const SandboxInfo &sandbox_info) -> json {
        return {{"id", sandbox_info.id},
                {"container_id", sandbox_info.runtime_id},
                {"runtime_backend", sandbox_info.runtime_backend},
                {"state", int(sandbox_info.state)},
                {"source", sandbox_info.options.src_repo.string()},
                {"revision", sandbox_info.options.revision},
                {"workspace", sandbox_info.options.ctr_repo.string()},
                {"cwd", sandbox_info.options.cwd_rlt.string()},
                {"memory", sandbox_info.options.memory_bytes},
                {"period", sandbox_info.options.cpu_period_us},
                {"quota", sandbox_info.options.cpu_quota_us},
                {"pids", sandbox_info.options.max_tasks},
                {"timeout", sandbox_info.options.cmd_timeout.count()},
                {"output", sandbox_info.options.max_output_bytes},
                {"file_limit", sandbox_info.options.max_file_bytes},
                {"environment",
                 sandbox_info.options.environment == Environment::HostTools ? "host-tools" : "minimal"},
                {"policy", int(sandbox_info.options.policy)},
                {"baseline", sandbox_info.base_commit},
                {"source_head", sandbox_info.source_head_at_creation},
                {"branch", sandbox_info.target_branch ? json(*sandbox_info.target_branch) : json(nullptr)},
                {"error", sandbox_info.last_error},
                {"rootless", sandbox_info.rootless},
                {"resource_limits_verified", sandbox_info.resource_limits_verified}};
    }

} // namespace

auto default_sandbox_root() -> fs::path {
    if (geteuid() == 0) {
        return "/var/lib/bbm-sandbox";
    }
    if (const auto *state_home_env = std::getenv("XDG_STATE_HOME")) {
        fs::path state_home(state_home_env);
        require(state_home.is_absolute(), "XDG_STATE_HOME must be absolute");
        return state_home / "bbm-sandbox";
    }
    const auto *user_account = getpwuid(geteuid());
    require(user_account != nullptr, "cannot determine manager user home");
    return fs::path(user_account->pw_dir) / ".local/state/bbm-sandbox";
}

Sandbox::Sandbox(const fs::path& root)
    : Sandbox(root, make_crun_backend()) {
}

Sandbox::Sandbox(const fs::path& root,
                 std::unique_ptr<RuntimeBackend> runtime)
    : root_(fs::absolute(root)), runtime_(std::move(runtime)) {
    require(runtime_ != nullptr, "sandbox runtime must be provided");
    if (fs::create_directories(root_)) {
        fs::permissions(root_, fs::perms::owner_all);
    }
    struct stat root_status{};
    require(lstat(root_.c_str(), &root_status) == 0 && S_ISDIR(root_status.st_mode) &&
                root_status.st_uid == geteuid() && !(root_status.st_mode & 0022),
            "manager root must be owned by the manager and not writable by group/others");
    root_ = fs::canonical(root_);
    runtime_->configure_state_directory(root_ / "runtime");
}

void Sandbox::save(const SandboxInfo &sandbox_info) {
    const auto temporary_record_path = sandbox_info.directory / "sandbox.json.tmp";
    std::ofstream record_stream(temporary_record_path);
    record_stream << encode_sandbox_info(sandbox_info).dump(2) << '\n';
    record_stream.close();
    require(static_cast<bool>(record_stream), "cannot save sandbox");
    fs::rename(temporary_record_path, sandbox_info.directory / "sandbox.json");
    fs::remove(sandbox_info.directory / "session.json");
}

auto Sandbox::load(const std::string &sandbox_id) -> SandboxInfo {
    valid_id(sandbox_id);
    SandboxInfo sandbox_info;

    sandbox_info.id = sandbox_id;
    sandbox_info.runtime_id = "bbm-sandbox-" + sandbox_id;
    sandbox_info.directory = root_ / sandbox_id;
    sandbox_info.bundle_dir = sandbox_info.directory / "bundle";
    sandbox_info.work_files_dir = sandbox_info.directory / "workspace/files";
    require(fs::symlink_status(sandbox_info.directory).type() == fs::file_type::directory,
            "unknown sandbox");

    const auto record_path = fs::exists(sandbox_info.directory / "sandbox.json")
        ? sandbox_info.directory / "sandbox.json"
        : sandbox_info.directory / "session.json"; // Legacy record migration.

    std::ifstream record_stream(record_path);

    json record_json;
    record_stream >> record_json;

    require(record_json.at("id") == sandbox_id &&
                record_json.at("container_id") == sandbox_info.runtime_id,
            "invalid sandbox record");
    sandbox_info.runtime_backend = record_json.value("runtime_backend", std::string("oci-crun"));
    require(sandbox_info.runtime_backend == runtime_->id(),
            "sandbox belongs to a different runtime backend");
    // Older records named the workspace adapter. Only their Git layout is supported.
    require(record_json.value("workspace_backend", std::string("git")) == "git",
            "unsupported legacy workspace format");
    sandbox_info.state = SandboxState(record_json.at("state").get<int>());
    sandbox_info.last_error = record_json.at("error");
    sandbox_info.rootless = record_json.value("rootless", false);
    sandbox_info.resource_limits_verified = record_json.value("resource_limits_verified", false);
    auto &options = sandbox_info.options;
    options.src_repo = record_json.at("source").get<std::string>();
    options.revision = record_json.at("revision");
    options.ctr_repo = record_json.at("workspace").get<std::string>();
    options.cwd_rlt = record_json.at("cwd").get<std::string>();
    options.memory_bytes = record_json.at("memory");
    options.cpu_period_us = record_json.at("period");
    options.cpu_quota_us = record_json.at("quota");
    options.max_tasks = record_json.at("pids");
    options.cmd_timeout = std::chrono::milliseconds(record_json.at("timeout").get<int>());
    options.max_output_bytes = record_json.at("output");
    options.max_file_bytes = record_json.value("file_limit", options.max_file_bytes);
    const auto environment = record_json.value("environment", std::string("minimal"));
    require(environment == "host-tools" || environment == "minimal", "invalid environment record");
    options.environment = environment == "host-tools" ? Environment::HostTools : Environment::Minimal;
    options.policy = MergePolicy(record_json.at("policy").get<int>());
    sandbox_info.base_commit = record_json.at("baseline");
    sandbox_info.source_head_at_creation = record_json.at("source_head");
    if (!record_json.at("branch").is_null()) {
        sandbox_info.target_branch = record_json.at("branch").get<std::string>();
    }
    return sandbox_info;
}

auto Sandbox::create(const Options &options) -> SandboxInfo {
    require(!info_.has_value(), "Sandbox is already bound; create a separate object");
    auto resolved_options = options;

    require(resolved_options.policy == MergePolicy::ReviewOnly,
            "automatic merge is not implemented in phase one");

    require(resolved_options.environment == Environment::HostTools ||
                resolved_options.environment == Environment::Minimal,
            "invalid environment");

    require(resolved_options.ctr_repo.is_absolute() &&
                resolved_options.ctr_repo.lexically_normal() == resolved_options.ctr_repo,
            "workspace must be an absolute normalized container path");
    // Dedicated mount target, avoiding runtime/system paths and their descendants.

    require(resolved_options.ctr_repo != "/" &&
                resolved_options.ctr_repo.string().find('\0') == std::string::npos,
            "invalid workspace mount path");

    relative_path(resolved_options.cwd_rlt);

    require(resolved_options.cmd_timeout.count() > 0 &&
                resolved_options.cmd_timeout.count() <= 3600000 &&
                resolved_options.max_output_bytes > 0 &&
                resolved_options.max_output_bytes <= 64 * 1024 * 1024 &&
                resolved_options.max_file_bytes > 0 &&
                resolved_options.max_file_bytes <= 64 * 1024 * 1024,
            "invalid execution limits");

    require(resolved_options.memory_bytes >= 16 * 1024 * 1024 &&
                resolved_options.memory_bytes <= static_cast<size_t>(std::numeric_limits<int64_t>::max()) &&
                resolved_options.max_tasks >= 2 && resolved_options.max_tasks <= 4096 &&
                resolved_options.cpu_period_us >= 1000 &&
                resolved_options.cpu_period_us <= 1000000 &&
                (resolved_options.cpu_quota_us == 0 || resolved_options.cpu_quota_us >= 1000) &&
                resolved_options.cpu_quota_us <= static_cast<size_t>(std::numeric_limits<int64_t>::max()),
            "invalid resource limits");


    resolved_options.src_repo = fs::canonical(resolved_options.src_repo);

    runtime_->validate_options(resolved_options);


    SandboxInfo sandbox_info;
    sandbox_info.id = new_id();
    sandbox_info.runtime_id = "bbm-sandbox-" + sandbox_info.id;
    sandbox_info.options = resolved_options;
    sandbox_info.runtime_backend = runtime_->id();
    sandbox_info.directory = root_ / sandbox_info.id;
    sandbox_info.bundle_dir = sandbox_info.directory / "bundle";

    require(fs::create_directory(sandbox_info.directory), "sandbox ID collision");
    fs::permissions(sandbox_info.directory, fs::perms::owner_all);

    SandboxLock lock(sandbox_info.directory / "lock");
    bool runtime_start_attempted = false;

    try {
        const auto workspace = Workspace::create(
            resolved_options.src_repo, sandbox_info.directory / "workspace", resolved_options.revision);
        sandbox_info.options.src_repo = workspace.source_repository();
        sandbox_info.work_files_dir = workspace.files_path();
        sandbox_info.base_commit = workspace.baseline();
        sandbox_info.source_head_at_creation = workspace.source_head();
        sandbox_info.target_branch = workspace.source_branch();
        runtime_->prepare(sandbox_info);
        save(sandbox_info);
        runtime_start_attempted = true;
        runtime_->start(sandbox_info);
        sandbox_info.state = SandboxState::Active;
        save(sandbox_info);
        info_ = sandbox_info;
        return sandbox_info;
    } catch (const std::exception &error) {
        sandbox_info.state = SandboxState::Failed;
        sandbox_info.last_error = error.what();
        if (runtime_start_attempted) {
            try {
                runtime_->stop(sandbox_info);
            } catch (const std::exception &cleanup_error) {
                sandbox_info.last_error += "; cleanup failed: " + std::string(cleanup_error.what());
            }
        }
        save(sandbox_info);
        info_ = sandbox_info;
        throw std::runtime_error("sandbox " + sandbox_info.id + " failed: " + sandbox_info.last_error);
    }
}
auto Sandbox::execute(const std::vector<std::string> &argv, OutputCallback on_output) -> Result {
    return execute(CommandRequest{argv, std::nullopt, {}}, std::move(on_output));
}
auto Sandbox::execute(const CommandRequest &request, OutputCallback on_output) -> Result {
    const auto &sandbox_id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(sandbox_id);
    SandboxLock lock(root_ / sandbox_id / "lock");
    auto sandbox_info = load(sandbox_id);
    return execute_locked(sandbox_info, request, std::move(on_output));
}

auto Sandbox::execute_locked(SandboxInfo &sandbox_info, const CommandRequest &request,
                               OutputCallback on_output) -> Result {
    require(sandbox_info.state == SandboxState::Active, "sandbox is not active");
    require(!request.argv.empty() && !request.argv[0].empty() && request.argv[0][0] == '/',
            "command must use an absolute container path");

    size_t argument_bytes = 0;
    for (const auto &argument : request.argv) {
        require(argument.find('\0') == std::string::npos, "NUL in argument");
        argument_bytes += argument.size();
    }
    require(request.argv.size() <= 1024 && argument_bytes <= 32 * 1024,
            "command arguments too large");
    require(request.stdin_data.size() <= sandbox_info.options.max_file_bytes,
            "stdin exceeds file limit");

    const auto working_directory = get_cwd(
        sandbox_info.options.ctr_repo,
        request.cwd_relative.value_or(sandbox_info.options.cwd_rlt));
        
    require_active(sandbox_info);

    Result result;
    try {
        const RuntimeCommand runtime_command{request.argv,
                                             working_directory,
                                             request.stdin_data,
                                             static_cast<int>(sandbox_info.options.cmd_timeout.count()),
                                             sandbox_info.options.max_output_bytes,
                                             std::move(on_output)};
        result = runtime_->execute(sandbox_info, runtime_command);
    } catch (...) {
        sandbox_info.state = SandboxState::Failed;
        sandbox_info.last_error = "execution or streaming failed: ";
        try {
            throw;
        } catch (const std::exception &error) {
            sandbox_info.last_error += error.what();
        } catch (...) {
            sandbox_info.last_error += "non-standard exception";
        }
        try {
            runtime_->stop(sandbox_info);
        } catch (const std::exception &cleanup_error) {
            sandbox_info.last_error += "; cleanup failed: " + std::string(cleanup_error.what());
        }
        save(sandbox_info);
        throw;
    }
    handle_execution_result(sandbox_info, result);
    return result;
}


auto Sandbox::cancel() -> bool {
    require(info_.has_value() && !destroyed_, "Sandbox must be bound and not destroyed");
    return runtime_->cancel();
}


void Sandbox::require_active(SandboxInfo &sandbox) {
    require(sandbox.state == SandboxState::Active, "sandbox is not active");
    const auto runtime_status = runtime_->status(sandbox);
    if (!runtime_status.verified || runtime_status.state != RuntimeState::Running) {
        sandbox.state = SandboxState::Failed;
        sandbox.last_error = "runtime is not running: " + runtime_status.detail;
        save(sandbox);
        throw std::runtime_error(sandbox.last_error);
    }
}

void Sandbox::handle_execution_result(SandboxInfo &sandbox, const Result &result) {
    if (!result.timed_out && !result.output_limited) {
        return;
    }
    sandbox.state = SandboxState::Failed;
    sandbox.last_error = result.timed_out ? "command timed out" : "command output limit exceeded";
    try {
        runtime_->stop(sandbox);
    } catch (const std::exception &cleanup_error) {
        sandbox.last_error += "; cleanup failed: " + std::string(cleanup_error.what());
    }
    save(sandbox);
}

auto Sandbox::read(const fs::path &container_path) -> std::string {
    const auto &sandbox_id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    require(container_path.is_absolute(), "read expects an absolute container path");
    valid_id(sandbox_id);

    SandboxLock lock(root_ / sandbox_id / "lock");
    auto sandbox_info = load(sandbox_id);
    const auto output_limit = std::min(sandbox_info.options.max_file_bytes,
                                       sandbox_info.options.max_output_bytes);
    require_active(sandbox_info);

    const auto result = runtime_->read(sandbox_info, container_path, output_limit);
    handle_execution_result(sandbox_info, result);
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox read failed: " + result.err);
    return result.out;
}

void Sandbox::write(const fs::path &container_path,
                    std::string_view content) {
    const auto &sandbox_id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    require(container_path.is_absolute(), "write expects an absolute container path");
    valid_id(sandbox_id);

    SandboxLock lock(root_ / sandbox_id / "lock");
    auto sandbox_info = load(sandbox_id);
    require(content.size() <= sandbox_info.options.max_file_bytes, "file exceeds write limit");
    require_active(sandbox_info);

    const auto result = runtime_->write(sandbox_info, container_path, content);
    handle_execution_result(sandbox_info, result);
    
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox write failed: " + result.err);
}

auto Sandbox::status() -> SandboxStatus {
    const auto &sandbox_id = this->id();
    if (destroyed_) return {*info_, "missing", {}};
    valid_id(sandbox_id);

    SandboxLock lock(root_ / sandbox_id / "lock");
    auto sandbox_info = load(sandbox_id);
    const auto runtime_status = runtime_->status(sandbox_info);
    SandboxStatus sandbox_status{sandbox_info, runtime_status.detail, runtime_status.error};
    
    if (sandbox_info.state == SandboxState::Active &&
        (!runtime_status.verified || runtime_status.state != RuntimeState::Running)) {
        
        sandbox_info.state = SandboxState::Failed;
        sandbox_info.last_error = "runtime is no longer running: " + runtime_status.detail;
        save(sandbox_info);
        sandbox_status.info = std::move(sandbox_info);
    }

    return sandbox_status;
}

auto Sandbox::get_status() -> SandboxState {
    return status().info.state;
}

auto Sandbox::get_changes() -> Changes {
    const auto &sandbox_id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(sandbox_id);
    SandboxLock lock(root_ / sandbox_id / "lock");

    auto sandbox_info = load(sandbox_id);
    const bool runtime_was_active = sandbox_info.state == SandboxState::Active;
    
    if (runtime_was_active) {
        runtime_->pause(sandbox_info);
    } else {
        const auto runtime_status = runtime_->status(sandbox_info);
        require(runtime_status.verified &&
                    (runtime_status.state == RuntimeState::Stopped ||
                     runtime_status.state == RuntimeState::Missing),
                "cannot verify runtime cleanup; refusing unstable diff");
    }
    
    auto resume_runtime = [&] () -> void {
        if (!runtime_was_active) {
            return;
        }
        try {
            runtime_->resume(sandbox_info);
        } catch (const std::exception &error) {
            sandbox_info.state = SandboxState::Failed;
            sandbox_info.last_error = "failed to resume runtime: " + std::string(error.what());
            save(sandbox_info);
            throw;
        }
    };

    Changes changes;
    try {
        runtime_->synchronize_workspace(sandbox_info);
        const auto workspace = Workspace::open(sandbox_info.directory / "workspace");
        changes = {workspace.status(), workspace.diff()};
    } catch (...) {
        resume_runtime();
        throw;
    }
    resume_runtime();

    return changes; // Preview only; no final commit or merge.
}

void Sandbox::stop() {
    const auto &sandbox_id = this->id();
    if (destroyed_) return;
    valid_id(sandbox_id);
    SandboxLock lock(root_ / sandbox_id / "lock");

    auto sandbox_info = load(sandbox_id);
    runtime_->stop(sandbox_info);
    sandbox_info.state = SandboxState::Stopped;
    save(sandbox_info);
}

void Sandbox::destroy() {
    const auto &sandbox_id = this->id();
    if (destroyed_) return;
    valid_id(sandbox_id);
    SandboxLock lock(root_ / sandbox_id / "lock");

    auto sandbox_info = load(sandbox_id);
    runtime_->stop(sandbox_info);
    const auto runtime_status = runtime_->status(sandbox_info);
    require(runtime_status.verified &&
                (runtime_status.state == RuntimeState::Stopped ||
                 runtime_status.state == RuntimeState::Missing),
            "runtime reclamation is not confirmed; sandbox retained");
    
    fs::remove_all(sandbox_info.directory);
    sandbox_info.state = SandboxState::Discarded;
    info_ = sandbox_info;
    destroyed_ = true;
}

auto Sandbox::id() const -> const std::string & {
    require(info_.has_value(), "Sandbox must be created or opened first");
    return info_->id;
}

auto Sandbox::info() -> SandboxInfo {
    const auto &sandbox_id = id();
    if (destroyed_) return *info_;
    SandboxLock lock(root_ / sandbox_id / "lock");
    return load(sandbox_id);
}

void Sandbox::open(const std::string &sandbox_id) {
    require(!info_.has_value(), "Sandbox is already bound");
    valid_id(sandbox_id);
    SandboxLock lock(root_ / sandbox_id / "lock");
    info_ = load(sandbox_id);
}
