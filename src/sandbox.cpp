#include "../include/sandbox.hpp"
#include "../include/lib.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {

    struct Lock {
        int fd;
        explicit Lock(const fs::path &path)
            : fd(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
            if (fd < 0) {
                throw std::runtime_error("cannot open sandbox lock");
            }
            if (flock(fd, LOCK_EX | LOCK_NB)) {
                close(fd);
                throw std::runtime_error("sandbox busy");
            }
        }
        ~Lock() {
            close(fd);
        }
        Lock(const Lock &) = delete;
    };

    json encode(const SandboxInfo &s) {
        return {{"id", s.id},
                {"container_id", s.runtime_id},
                {"runtime_backend", s.runtime_backend},
                {"workspace_backend", s.workspace_backend},
                {"state", int(s.state)},
                {"source", s.options.src_repo.string()},
                {"revision", s.options.revision},
                {"workspace", s.options.ctr_repo.string()},
                {"cwd", s.options.cwd_rlt.string()},
                {"memory", s.options.memory_bytes},
                {"period", s.options.cpu_period_us},
                {"quota", s.options.cpu_quota_us},
                {"pids", s.options.max_tasks},
                {"timeout", s.options.cmd_timeout.count()},
                {"output", s.options.max_output_bytes},
                {"file_limit", s.options.max_file_bytes},
                {"environment",
                 s.options.environment == Environment::HostTools ? "host-tools" : "minimal"},
                {"file_helper", s.helper_container_path.string()},
                {"policy", int(s.options.policy)},
                {"baseline", s.base_commit},
                {"source_head", s.source_head_at_creation},
                {"branch", s.target_branch ? json(*s.target_branch) : json(nullptr)},
                {"error", s.last_error},
                {"rootless", s.rootless},
                {"resource_limits_verified", s.resource_limits_verified}};
    }

} // namespace

fs::path default_sandbox_root() {
    if (geteuid() == 0) {
        return "/var/lib/bbm-sandbox";
    }
    if (const auto *state = std::getenv("XDG_STATE_HOME")) {
        fs::path path(state);
        require(path.is_absolute(), "XDG_STATE_HOME must be absolute");
        return path / "bbm-sandbox";
    }
    const auto *account = getpwuid(geteuid());
    require(account != nullptr, "cannot determine manager user home");
    return fs::path(account->pw_dir) / ".local/state/bbm-sandbox";
}

Sandbox::Sandbox(fs::path root)
    : Sandbox(std::move(root), make_libcrun_backend()) {
}

Sandbox::Sandbox(fs::path root,
                               std::unique_ptr<RuntimeBackend> runtime,
                               std::unique_ptr<WorkspaceBackend> workspace)
    : root_(fs::absolute(root)), runtime_(std::move(runtime)), workspace_(std::move(workspace)) {
    require(runtime_ != nullptr && workspace_ != nullptr, "sandbox backends must be provided");
    if (fs::create_directories(root_)) {
        fs::permissions(root_, fs::perms::owner_all);
    }
    struct stat st{};
    require(lstat(root_.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
                !(st.st_mode & 0022),
            "manager root must be owned by the manager and not writable by group/others");
    root_ = fs::canonical(root_);
    runtime_->configure_state_directory(root_ / "runtime");
}
void Sandbox::save(const SandboxInfo &s) {
    const auto tmp = s.directory / "sandbox.json.tmp";
    std::ofstream out(tmp);
    out << encode(s).dump(2) << '\n';
    out.close();
    require(bool(out), "cannot save sandbox");
    fs::rename(tmp, s.directory / "sandbox.json");
    fs::remove(s.directory / "session.json");
}
SandboxInfo Sandbox::load(const std::string &id) {
    valid_id(id);
    SandboxInfo s;
    s.id = id;
    s.runtime_id = "bbm-sandbox-" + id;
    s.directory = root_ / id;
    s.bundle_dir = s.directory / "bundle";
    s.work_files_dir = s.directory / "workspace/files";
    require(fs::symlink_status(s.directory).type() == fs::file_type::directory, "unknown sandbox");
    const auto record = fs::exists(s.directory / "sandbox.json")
        ? s.directory / "sandbox.json" : s.directory / "session.json"; // Legacy record migration.
    std::ifstream in(record);
    json j;
    in >> j;
    require(j.at("id") == id && j.at("container_id") == s.runtime_id, "invalid sandbox record");
    s.runtime_backend = j.value("runtime_backend", std::string("oci-crun"));
    s.workspace_backend = j.value("workspace_backend", std::string("git"));
    require(s.runtime_backend == runtime_->id(), "sandbox belongs to a different runtime backend");
    require(s.workspace_backend == workspace_->id(),
            "sandbox belongs to a different workspace backend");
    s.state = SandboxState(j.at("state").get<int>());
    s.last_error = j.at("error");
    s.rootless = j.value("rootless", false);
    s.resource_limits_verified = j.value("resource_limits_verified", false);
    s.helper_container_path = j.value("file_helper", std::string("/usr/bin/sandbox-io"));
    auto &o = s.options;
    o.src_repo = j.at("source").get<std::string>();
    o.revision = j.at("revision");
    o.ctr_repo = j.at("workspace").get<std::string>();
    o.cwd_rlt = j.at("cwd").get<std::string>();
    o.memory_bytes = j.at("memory");
    o.cpu_period_us = j.at("period");
    o.cpu_quota_us = j.at("quota");
    o.max_tasks = j.at("pids");
    o.cmd_timeout = std::chrono::milliseconds(j.at("timeout").get<int>());
    o.max_output_bytes = j.at("output");
    o.max_file_bytes = j.value("file_limit", o.max_file_bytes);
    const auto environment = j.value("environment", std::string("minimal"));
    require(environment == "host-tools" || environment == "minimal", "invalid environment record");
    o.environment = environment == "host-tools" ? Environment::HostTools : Environment::Minimal;
    o.policy = MergePolicy(j.at("policy").get<int>());
    s.base_commit = j.at("baseline");
    s.source_head_at_creation = j.at("source_head");
    if (!j.at("branch").is_null()) {
        s.target_branch = j.at("branch").get<std::string>();
    }
    return s;
}

SandboxInfo Sandbox::create(const Options &options) {
    require(!info_.has_value(), "Sandbox is already bound; create a separate object");
    auto o = options;
    require(o.policy == MergePolicy::ReviewOnly, "automatic merge is not implemented in phase one");
    require(o.environment == Environment::HostTools || o.environment == Environment::Minimal,
            "invalid environment");
    require(o.ctr_repo.is_absolute() && o.ctr_repo.lexically_normal() == o.ctr_repo,
            "workspace must be an absolute normalized container path");
    // Dedicated mount target, avoiding runtime/system paths and their descendants.
    require(o.ctr_repo != "/" && o.ctr_repo.string().find('\0') == std::string::npos,
            "invalid workspace mount path");
    relative_path(o.cwd_rlt);
    require(o.cmd_timeout.count() > 0 && o.cmd_timeout.count() <= 3600000 &&
                o.max_output_bytes > 0 && o.max_output_bytes <= 64 * 1024 * 1024 &&
                o.max_file_bytes > 0 && o.max_file_bytes <= 64 * 1024 * 1024,
            "invalid execution limits");
    require(o.memory_bytes >= 16 * 1024 * 1024 &&
                o.memory_bytes <= size_t(std::numeric_limits<int64_t>::max()) && o.max_tasks >= 2 &&
                o.max_tasks <= 4096 && o.cpu_period_us >= 1000 && o.cpu_period_us <= 1000000 &&
                (o.cpu_quota_us == 0 || o.cpu_quota_us >= 1000) &&
                o.cpu_quota_us <= size_t(std::numeric_limits<int64_t>::max()),
            "invalid resource limits");
    o.src_repo = fs::canonical(o.src_repo);
    runtime_->validate_options(o);
    SandboxInfo s;
    s.id = new_id();
    s.runtime_id = "bbm-sandbox-" + s.id;
    s.options = o;
    s.runtime_backend = runtime_->id();
    s.workspace_backend = workspace_->id();
    s.directory = root_ / s.id;
    s.bundle_dir = s.directory / "bundle";
    require(fs::create_directory(s.directory), "sandbox ID collision");
    fs::permissions(s.directory, fs::perms::owner_all);
    Lock lock(s.directory / "lock");
    bool attempted = false;
    try {
        const auto snapshot = workspace_->create(o.src_repo, s.directory / "workspace", o.revision);
        s.options.src_repo = snapshot.source_repository;
        s.work_files_dir = snapshot.files_directory;
        s.base_commit = snapshot.baseline;
        s.source_head_at_creation = snapshot.source_head;
        s.target_branch = snapshot.source_branch;
        runtime_->prepare(s);
        save(s);
        attempted = true;
        runtime_->start(s);
        s.state = SandboxState::Active;
        save(s);
        info_ = s;
        return s;
    } catch (const std::exception &e) {
        s.state = SandboxState::Failed;
        s.last_error = e.what();
        if (attempted) {
            try {
                runtime_->stop(s);
            } catch (const std::exception &cleanup) {
                s.last_error += "; cleanup failed: " + std::string(cleanup.what());
            }
        }
        save(s);
        info_ = s;
        throw std::runtime_error("sandbox " + s.id + " failed: " + s.last_error);
    }
}
Result Sandbox::execute(const std::vector<std::string> &argv) {
    return execute(CommandRequest{argv, std::nullopt, {}});
}
Result Sandbox::execute(const CommandRequest &request) {
    const auto &id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    return execute_locked(s, request);
}
Result Sandbox::execute_locked(SandboxInfo &s, const CommandRequest &request) {
    require(s.state == SandboxState::Active, "sandbox is not active");
    require(!request.argv.empty() && !request.argv[0].empty() && request.argv[0][0] == '/',
            "command must use an absolute container path");
    size_t bytes = 0;
    for (auto &arg : request.argv) {
        require(arg.find('\0') == std::string::npos, "NUL in argument");
        bytes += arg.size();
    }
    require(bytes <= 128 * 1024, "command arguments too large");
    require(request.stdin_data.size() <= s.options.max_file_bytes, "stdin exceeds file limit");
    const auto cwd = get_cwd(s.options.ctr_repo, request.cwd_relative.value_or(s.options.cwd_rlt));
    require_active(s);
    const auto result = runtime_->execute(s,
                                          RuntimeCommand{request.argv,
                                                         cwd,
                                                         request.stdin_data,
                                                         int(s.options.cmd_timeout.count()),
                                                         s.options.max_output_bytes});
    handle_execution_result(s, result);
    return result;
}

void Sandbox::require_active(SandboxInfo &sandbox) {
    require(sandbox.state == SandboxState::Active, "sandbox is not active");
    const auto status = runtime_->status(sandbox);
    if (!status.verified || status.state != RuntimeState::Running) {
        sandbox.state = SandboxState::Failed;
        sandbox.last_error = "runtime is not running: " + status.detail;
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
    } catch (const std::exception &cleanup) {
        sandbox.last_error += "; cleanup failed: " + std::string(cleanup.what());
    }
    save(sandbox);
}
std::string Sandbox::read(const fs::path &container_path) {
    const auto &id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    require(container_path.is_absolute(), "read expects an absolute container path");
    const auto limit = std::min(s.options.max_file_bytes, s.options.max_output_bytes);
    require_active(s);
    const auto result = runtime_->read(s, container_path, limit);
    handle_execution_result(s, result);
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox read failed: " + result.err);
    return result.out;
}
void Sandbox::write(const fs::path &container_path,
                           std::string_view content) {
    const auto &id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    require(container_path.is_absolute(), "write expects an absolute container path");
    require(content.size() <= s.options.max_file_bytes, "file exceeds write limit");
    require_active(s);
    const auto result = runtime_->write(s, container_path, content);
    handle_execution_result(s, result);
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox write failed: " + result.err);
}
SandboxStatus Sandbox::status() {
    const auto &id = this->id();
    if (destroyed_) return {*info_, "missing", {}};
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    const auto status = runtime_->status(s);
    SandboxStatus result{s, status.detail, status.error};
    if (s.state == SandboxState::Active &&
        (!status.verified || status.state != RuntimeState::Running)) {
        s.state = SandboxState::Failed;
        s.last_error = "runtime is no longer running: " + status.detail;
        save(s);
        result.info = s;
    }
    return result;
}
SandboxState Sandbox::get_status() {
    return status().info.state;
}
Changes Sandbox::get_changes() {
    const auto &id = this->id();
    require(!destroyed_, "Sandbox has been destroyed");
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    const bool active = s.state == SandboxState::Active;
    if (active) {
        runtime_->pause(s);
    } else {
        const auto status = runtime_->status(s);
        require(status.verified && (status.state == RuntimeState::Stopped ||
                                    status.state == RuntimeState::Missing),
                "cannot verify runtime cleanup; refusing unstable diff");
    }
    auto resume = [&] {
        if (!active) {
            return;
        }
        try {
            runtime_->resume(s);
        } catch (const std::exception &error) {
            s.state = SandboxState::Failed;
            s.last_error = "failed to resume runtime: " + std::string(error.what());
            save(s);
            throw;
        }
    };
    Changes changes;
    try {
        runtime_->synchronize_workspace(s);
        changes = workspace_->inspect(s.directory / "workspace");
    } catch (...) {
        resume();
        throw;
    }
    resume();
    return changes; // Preview only; no final commit or merge.
}
void Sandbox::stop() {
    const auto &id = this->id();
    if (destroyed_) return;
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    runtime_->stop(s);
    s.state = SandboxState::Stopped;
    save(s);
}

void Sandbox::destroy() {
    const auto &id = this->id();
    if (destroyed_) return;
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto sandbox = load(id);
    runtime_->stop(sandbox);
    const auto status = runtime_->status(sandbox);
    require(status.verified && (status.state == RuntimeState::Stopped ||
                                status.state == RuntimeState::Missing),
            "runtime reclamation is not confirmed; sandbox retained");
    fs::remove_all(sandbox.directory);
    sandbox.state = SandboxState::Discarded;
    info_ = sandbox;
    destroyed_ = true;
}

const std::string &Sandbox::id() const {
    require(info_.has_value(), "Sandbox must be created or opened first");
    return info_->id;
}

SandboxInfo Sandbox::info() {
    const auto &bound_id = id();
    if (destroyed_) return *info_;
    Lock lock(root_ / bound_id / "lock");
    return load(bound_id);
}

void Sandbox::open(const std::string &bound_id) {
    require(!info_.has_value(), "Sandbox is already bound");
    valid_id(bound_id);
    Lock lock(root_ / bound_id / "lock");
    info_ = load(bound_id);
}
