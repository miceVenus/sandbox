#include "sandbox.hpp"
#include "bundle.hpp"
#include "host_tools.hpp"
#include "lib.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
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
                throw std::runtime_error("cannot open session lock");
            }
            if (flock(fd, LOCK_EX | LOCK_NB)) {
                close(fd);
                throw std::runtime_error("session busy");
            }
        }
        ~Lock() {
            close(fd);
        }
        Lock(const Lock &) = delete;
    };

    json encode(const Session &s) {
        return {{"id", s.s_id},
                {"container_id", s.c_id},
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

    std::string source_git(const fs::path &repo,
                           const std::vector<std::string> &command,
                           bool optional = false) {
        std::vector<std::string> args{
            "/usr/bin/git", "-c", "safe.directory=" + repo.string(), "-C", repo.string()};
        args.insert(args.end(), command.begin(), command.end());
        auto r = run_process(args, 10000);
        if (optional && r.runtime_status == 1) {
            return {};
        }
        require(r.runtime_status == 0 && !r.timed_out && !r.output_limited,
                "cannot read source Git state: " + r.err);
        return trim(r.out);
    }

    void check_rootless_cgroups(const Options &options) {
        require(fs::exists("/sys/fs/cgroup/cgroup.controllers"),
                "rootless resource limits require cgroup v2");
        const auto user_cgroup = fs::path("/sys/fs/cgroup/user.slice") /
                                 ("user-" + std::to_string(geteuid()) + ".slice") /
                                 ("user@" + std::to_string(geteuid()) + ".service");
        // Standard systemd login layout: diagnose missing delegation before creating
        // a session. Other layouts are checked against the actual container below.
        std::ifstream controllers(user_cgroup / "cgroup.controllers");
        if (controllers) {
            std::string content((std::istreambuf_iterator<char>(controllers)), {});
            require(content.find("memory") != std::string::npos &&
                        content.find("pids") != std::string::npos,
                    "systemd user session must delegate memory and pids controllers");
            require(options.cpu_quota_us == 0 || content.find("cpu") != std::string::npos,
                    "CPU controller is not delegated to your systemd user session; delegate cpu or "
                    "explicitly use --cpu-quota-us 0 for debugging (no CPU quota)");
        }
    }
    void verify_resources(const Session &s, const json &state) {
        const auto pid = state.at("pid").get<int>();
        require(pid > 0, "crun did not return a container PID");
        std::ifstream groups(fs::path("/proc") / std::to_string(pid) / "cgroup");
        std::string line, path;
        while (std::getline(groups, line)) {
            if (line.rfind("0::/", 0) == 0) {
                path = line.substr(3);
            }
        }
        require(!path.empty() && path.find("bbm-sandbox-" + s.s_id) != std::string::npos,
                "cannot locate the container's dedicated cgroup v2");
        const auto cgroup = fs::path("/sys/fs/cgroup") / fs::path(path).relative_path();
        auto number = [&](const char *file, size_t expected) {
            std::ifstream in(cgroup / file);
            std::string value;
            in >> value;
            require(bool(in) && value == std::to_string(expected),
                    std::string("requested cgroup limit not applied: ") + file +
                        " (check systemd delegation)");
        };
        number("memory.max", s.options.memory_bytes);
        number("memory.swap.max", 0);
        number("pids.max", s.options.max_tasks);
        if (s.options.cpu_quota_us != 0) {
            std::ifstream cpu(cgroup / "cpu.max");
            std::string quota, period;
            cpu >> quota >> period;
            require(bool(cpu) && quota == std::to_string(s.options.cpu_quota_us) &&
                        period == std::to_string(s.options.cpu_period_us),
                    "requested CPU quota not applied; delegate cpu or explicitly use "
                    "--cpu-quota-us 0 for debugging");
        }
    }
} // namespace

fs::path default_manager_root() {
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

SandboxManager::SandboxManager(fs::path root, std::string runtime_binary)
    : root_(fs::absolute(root)), runtime_(root_ / "runtime", runtime_binary) {
    if (fs::create_directories(root_)) {
        fs::permissions(root_, fs::perms::owner_all);
    }
    struct stat st{};
    require(lstat(root_.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
                !(st.st_mode & 0022),
            "manager root must be owned by the manager and not writable by group/others");
    root_ = fs::canonical(root_);
    runtime_ = CrunClient(root_ / "runtime", std::move(runtime_binary), geteuid() != 0);
    fs::create_directories(root_ / "runtime");
}
void SandboxManager::save(const Session &s) {
    const auto tmp = s.s_dir / "session.json.tmp";
    std::ofstream out(tmp);
    out << encode(s).dump(2) << '\n';
    out.close();
    require(bool(out), "cannot save session");
    fs::rename(tmp, s.s_dir / "session.json");
}
Session SandboxManager::load(const std::string &id) {
    valid_id(id);
    Session s;
    s.s_id = id;
    s.c_id = "bbm-sandbox-" + id;
    s.s_dir = root_ / id;
    s.bundle_dir = s.s_dir / "bundle";
    s.work_files_dir = s.s_dir / "workspace/files";
    require(fs::symlink_status(s.s_dir).type() == fs::file_type::directory, "unknown session");
    std::ifstream in(s.s_dir / "session.json");
    json j;
    in >> j;
    require(j.at("id") == id && j.at("container_id") == s.c_id, "invalid session record");
    s.state = SessionState(j.at("state").get<int>());
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

Session SandboxManager::create_session(const Options &options) {
    auto o = options;
    require(o.policy == MergePolicy::ReviewOnly, "automatic merge is not implemented in phase one");
    require(o.environment == Environment::HostTools || o.environment == Environment::Minimal,
            "invalid environment");
    require(o.ctr_repo.is_absolute() && o.ctr_repo.lexically_normal() == o.ctr_repo,
            "workspace must be an absolute normalized container path");
    // Dedicated mount target, avoiding runtime/system paths and their descendants.
    require(o.ctr_repo != "/" && o.ctr_repo.string().find('\0') == std::string::npos,
            "invalid workspace mount path");
    auto top = *++o.ctr_repo.begin();
    require(top != "" && top != "bin" && top != "dev" && top != "proc" && top != "sys" &&
                top != "tmp" && top != "etc" && top != "usr" && top != "lib" && top != "sbin" &&
                top != "lib64" && top != "build" && top != "env" && top != "cache" &&
                top != "sandbox-tools",
            "reserved workspace mount path");
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
    if (geteuid() != 0) {
        check_rootless_cgroups(o);
    }
    Session s;
    s.s_id = new_id();
    s.c_id = "bbm-sandbox-" + s.s_id;
    s.options = o;
    s.rootless = geteuid() != 0;
    s.s_dir = root_ / s.s_id;
    s.bundle_dir = s.s_dir / "bundle";
    require(fs::create_directory(s.s_dir), "session ID collision");
    fs::permissions(s.s_dir, fs::perms::owner_all);
    Lock lock(s.s_dir / "lock");
    bool attempted = false;
    try {
        auto w = GitWorkspace::create(o.src_repo, s.s_dir / "workspace", o.revision);
        o.src_repo = w.source_repository();
        s.options.src_repo = o.src_repo;
        if (o.environment == Environment::HostTools) {
            validate_host_tools(o.src_repo, root_);
        }
        s.work_files_dir = w.files_path();
        s.base_commit = w.baseline();
        s.source_head_at_creation = source_git(o.src_repo, {"rev-parse", "HEAD"});
        auto branch = source_git(o.src_repo, {"symbolic-ref", "--quiet", "--short", "HEAD"}, true);
        if (!branch.empty()) {
            s.target_branch = branch;
        }
        prepare_bundle(s);
        save(s);
        attempted = true;
        checked(runtime_.start(s.c_id, s.bundle_dir.string()));
        auto r = runtime_.state(s.c_id);
        checked(r);
        auto state = json::parse(r.out);
        require(state.at("id") == s.c_id && state.at("status") == "running",
                "container failed to enter running state");
        verify_resources(s, state);
        s.resource_limits_verified = true;
        s.state = SessionState::Active;
        save(s);
        return s;
    } catch (const std::exception &e) {
        s.state = SessionState::Failed;
        s.last_error = e.what();
        if (attempted && !runtime_.destroy(s.c_id)) {
            s.last_error += "; container cleanup failed";
        }
        save(s);
        throw std::runtime_error("session " + s.s_id + " failed: " + s.last_error);
    }
}
Result SandboxManager::execute(const std::string &id, const std::vector<std::string> &argv) {
    return execute(id, CommandRequest{argv, std::nullopt, {}});
}
Result SandboxManager::execute(const std::string &id, const CommandRequest &request) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    return execute_locked(s, request);
}
Result SandboxManager::execute_locked(Session &s, const CommandRequest &request) {
    require(s.state == SessionState::Active, "session is not active");
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
    auto state = runtime_.state(s.c_id);
    if (state.runtime_status != 0 || state.timed_out || state.output_limited ||
        json::parse(state.out).at("status") != "running") {
        s.state = SessionState::Failed;
        s.last_error = "container is not running";
        save(s);
        throw std::runtime_error(s.last_error);
    }
    auto r = runtime_.exec(s.c_id,
                           cwd.string(),
                           request.argv,
                           int(s.options.cmd_timeout.count()),
                           s.options.max_output_bytes,
                           request.stdin_data);
    if (r.timed_out || r.output_limited) {
        s.state = SessionState::Failed;
        s.last_error = r.timed_out ? "command timed out" : "command output limit exceeded";
        if (!runtime_.destroy(s.c_id)) {
            s.last_error += "; container cleanup failed";
        }
        save(s);
    }
    return r;
}
std::string SandboxManager::read(const std::string &id, const fs::path &container_path) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    require(container_path.is_absolute(), "read expects an absolute container path");
    const auto limit = std::min(s.options.max_file_bytes, s.options.max_output_bytes);
    CommandRequest request;
    request.cwd_relative = ".";
    request.argv = {s.helper_container_path.string(),
                    "read",
                    s.options.ctr_repo.string(),
                    container_path.string(),
                    std::to_string(limit)};
    const auto result = execute_locked(s, request);
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox read failed: " + result.err);
    return result.out;
}
void SandboxManager::write(const std::string &id,
                           const fs::path &container_path,
                           std::string_view content) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    require(container_path.is_absolute(), "write expects an absolute container path");
    require(content.size() <= s.options.max_file_bytes, "file exceeds write limit");
    CommandRequest request;
    request.cwd_relative = ".";
    request.argv = {s.helper_container_path.string(),
                    "write",
                    s.options.ctr_repo.string(),
                    container_path.string(),
                    std::to_string(s.options.max_file_bytes)};
    request.stdin_data.assign(content);
    const auto result = execute_locked(s, request);
    require(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
            "sandbox write failed: " + result.err);
}
SessionStatus SandboxManager::get_session_status(const std::string &id) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    auto r = runtime_.state(s.c_id);
    SessionStatus result{s, "unknown", r.err};
    if (r.runtime_status == 0 && !r.timed_out && !r.output_limited) {
        result.container_status = json::parse(r.out).at("status");
    }
    if (s.state == SessionState::Active && result.container_status != "running") {
        s.state = SessionState::Failed;
        s.last_error = "container is no longer running: " + result.container_status;
        save(s);
        result.session = s;
    }
    return result;
}
SessionState SandboxManager::get_status(const std::string &id) {
    return get_session_status(id).session.state;
}
Changes SandboxManager::get_changes(const std::string &id) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    const bool active = s.state == SessionState::Active;
    if (active) {
        checked(runtime_.pause(s.c_id));
    } else {
        auto r = runtime_.state(s.c_id);
        if (r.runtime_status == 0) {
            require(json::parse(r.out).at("status") == "stopped",
                    "container must be stopped before inspecting failed session");
        } else {
            require(!r.timed_out && !r.output_limited && !fs::exists(root_ / "runtime" / s.c_id),
                    "cannot verify container cleanup; refusing unstable diff");
        }
    }
    auto resume = [&] {
        if (!active) {
            return;
        }
        auto r = runtime_.resume(s.c_id);
        if (r.runtime_status != 0 || r.timed_out || r.output_limited) {
            s.state = SessionState::Failed;
            s.last_error = "failed to resume container: " + r.err;
            save(s);
            throw std::runtime_error(s.last_error);
        }
    };
    Changes changes;
    try {
        auto w = GitWorkspace::open(s.s_dir / "workspace");
        changes.status = w.status();
        changes.diff = w.diff();
    } catch (...) {
        resume();
        throw;
    }
    resume();
    return changes; // Preview only; no final commit or merge.
}
void SandboxManager::stop_session(const std::string &id) {
    valid_id(id);
    Lock lock(root_ / id / "lock");
    auto s = load(id);
    auto state = runtime_.state(s.c_id);
    const bool absent = state.runtime_status != 0 && !state.timed_out && !state.output_limited &&
                        !fs::exists(root_ / "runtime" / s.c_id);
    if (!absent) {
        require(runtime_.destroy(s.c_id), "container cleanup failed; session retained");
    }
    s.state = SessionState::Stopped;
    save(s);
}
int session_cli(int argc, char **argv) {
    int i = 0;
    fs::path root = default_manager_root();
    if (argc >= 2 && std::string(argv[0]) == "--root") {
        root = argv[1];
        i = 2;
    }
    require(i < argc,
            "usage: session [--root DIR] create REPO | exec ID [--cwd REL] -- /bin/CMD ... | state "
            "ID | changes ID | stop ID | read ID PATH | write ID PATH < FILE");
    SandboxManager manager(root);
    std::string action = argv[i++];
    require(i < argc, "missing repository or ID");
    std::string value = argv[i++];
    if (action == "create") {
        Options o;
        o.src_repo = value;
        while (i < argc) {
            std::string flag = argv[i++];
            require(i < argc, "missing option value");
            std::string v = argv[i++];
            if (flag == "--workspace") {
                o.ctr_repo = v;
            } else if (flag == "--environment") {
                require(v == "host-tools" || v == "minimal", "unknown environment: " + v);
                o.environment = v == "host-tools" ? Environment::HostTools : Environment::Minimal;
            } else if (flag == "--cwd") {
                o.cwd_rlt = v;
            } else if (flag == "--revision") {
                o.revision = v;
            } else if (flag == "--timeout-ms") {
                o.cmd_timeout = std::chrono::milliseconds(std::stoll(v));
            } else if (flag == "--cpu-quota-us") {
                require(!v.empty() && v.find_first_not_of("0123456789") == std::string::npos,
                        "invalid CPU quota");
                o.cpu_quota_us = std::stoull(v);
            } else {
                throw std::runtime_error("unknown create option: " + flag);
            }
        }
        std::cout << encode(manager.create_session(o)).dump(2) << '\n';
        return 0;
    }
    if (action == "exec") {
        CommandRequest req;
        if (i + 1 < argc && std::string(argv[i]) == "--cwd") {
            req.cwd_relative = argv[i + 1];
            i += 2;
        }
        require(i < argc && std::string(argv[i++]) == "--", "expected -- before command");
        req.argv.assign(argv + i, argv + argc);
        auto r = manager.execute(value, req);
        std::cout << r.out;
        std::cerr << r.err;
        return r.timed_out ? 124 : r.output_limited ? 125 : r.runtime_status;
    }
    if (action == "read" || action == "write") {
        require(i + 1 == argc, "expected one absolute container file path");
        const fs::path path = argv[i];
        if (action == "read") {
            const auto content = manager.read(value, path);
            std::cout.write(content.data(), content.size());
        } else {
            const auto limit = manager.get_session_status(value).session.options.max_file_bytes;
            std::string content;
            std::array<char, 16384> buffer{};
            while (std::cin.read(buffer.data(), buffer.size()) || std::cin.gcount()) {
                const auto count = static_cast<size_t>(std::cin.gcount());
                require(count <= limit - content.size(), "file exceeds write limit");
                content.append(buffer.data(), count);
            }
            require(!std::cin.bad(), "cannot read stdin");
            manager.write(value, path, content);
        }
        return 0;
    }
    require(i == argc, "unexpected arguments");
    if (action == "state") {
        auto s = manager.get_session_status(value);
        auto j = encode(s.session);
        j["container_status"] = s.container_status;
        j["runtime_error"] = s.runtime_error;
        std::cout << j.dump(2) << '\n';
    } else if (action == "changes") {
        auto c = manager.get_changes(value);
        std::cout << json({{"final", false}, {"status", c.status}, {"diff", c.diff}}).dump(2)
                  << '\n';
    } else if (action == "stop") {
        manager.stop_session(value);
    } else {
        throw std::runtime_error("unknown session action");
    }
    return 0;
}
