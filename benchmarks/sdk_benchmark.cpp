#include "../include/sandbox.hpp"
#include "../include/virtualization/container/crun_worker_client.hpp"
#include "../include/virtualization/microvm/libkrun_runtime.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
static std::string read_text(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static uint64_t read_number(const fs::path &path) {
    return std::stoull(read_text(path));
}
static json disk_usage(const fs::path &path) {
    uint64_t logical = 0, allocated = 0, entries = 0;
    auto add = [&](const fs::path &entry) {
        struct stat st{};
        if (lstat(entry.c_str(), &st) != 0) throw std::runtime_error("lstat failed: " + entry.string());
        logical += st.st_size;
        allocated += uint64_t(st.st_blocks) * 512;
        ++entries;
    };
    if (fs::exists(path)) {
        add(path);
        // No symlink traversal. HostTools bind mounts live in the VMM namespace.
        for (const auto &entry : fs::recursive_directory_iterator(path)) add(entry.path());
    }
    return {{"logical_bytes", logical}, {"allocated_bytes", allocated}, {"entries", entries}};
}
static json sandbox_disk(const SandboxInfo &s) {
    json result{{"total", disk_usage(s.directory)}};
    for (const auto *part : {"guest", "vmm", "workspace", "runtime-data", "vm-control"}) {
        result[part] = disk_usage(s.directory / part);
    }
    return result;
}

// Timing decorators keep measurement code outside the production backend.
class MeasuredRuntime : public RuntimeBackend {
  public:
    std::unique_ptr<RuntimeBackend> inner = make_libkrun_backend();
    double prepare_ms = 0, start_ms = 0;
    std::string id() const override { return inner->id(); }
    void configure_state_directory(const fs::path &p) override { inner->configure_state_directory(p); }
    void validate_options(const Options &o) override { inner->validate_options(o); }
    void prepare(SandboxInfo &s) override {
        const auto begin = Clock::now();
        inner->prepare(s);
        prepare_ms = elapsed(begin);
    }
    void start(SandboxInfo &s) override {
        const auto begin = Clock::now();
        inner->start(s);
        start_ms = elapsed(begin);
    }
    RuntimeStatus status(const SandboxInfo &s) override { return inner->status(s); }
    Result execute(const SandboxInfo &s, const RuntimeCommand &c) override { return inner->execute(s, c); }
    Result read(const SandboxInfo &s, const fs::path &p, size_t n) override { return inner->read(s, p, n); }
    Result write(const SandboxInfo &s, const fs::path &p, std::string_view v) override { return inner->write(s, p, v); }
    void pause(const SandboxInfo &s) override { inner->pause(s); }
    void resume(const SandboxInfo &s) override { inner->resume(s); }
    void synchronize_workspace(const SandboxInfo &s) override { inner->synchronize_workspace(s); }
    void stop(const SandboxInfo &s) override { inner->stop(s); }
};
class MeasuredWorkspace : public WorkspaceBackend {
  public:
    std::unique_ptr<WorkspaceBackend> inner = make_git_workspace_backend();
    double create_ms = 0;
    std::string id() const override { return inner->id(); }
    WorkspaceSnapshot create(const fs::path &source, const fs::path &directory,
                             const std::string &revision) override {
        auto begin = Clock::now();
        auto result = inner->create(source, directory, revision);
        create_ms = elapsed(begin);
        return result;
    }
    Changes inspect(const fs::path &p) override { return inner->inspect(p); }
};
static fs::path cgroup_of(int pid) {
    std::istringstream in(read_text(fs::path("/proc") / std::to_string(pid) / "cgroup"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("0::/", 0) == 0) return fs::path("/sys/fs/cgroup") / line.substr(4);
    }
    throw std::runtime_error("VMM cgroup v2 missing");
}
static json memory_snapshot(int pid, const fs::path &cgroup) {
    json result{{"cgroup_current_bytes", read_number(cgroup / "memory.current")},
                {"cgroup_peak_bytes", read_number(cgroup / "memory.peak")},
                {"cgroup_swap_bytes", read_number(cgroup / "memory.swap.current")},
                {"cgroup_limit_bytes", read_number(cgroup / "memory.max")}};
    std::istringstream stat(read_text(cgroup / "memory.stat"));
    std::string key;
    uint64_t bytes;
    while (stat >> key >> bytes) {
        if (key == "anon" || key == "file" || key == "kernel") result["cgroup_" + key + "_bytes"] = bytes;
    }
    const auto proc = fs::path("/proc") / std::to_string(pid);
    // ptrace permissions may prohibit smaps_rollup for the rootless namespace's VMM.
    for (const auto *file : {"status", "smaps_rollup"}) {
        std::ifstream in(proc / file);
        if (!in) { result[std::string(file) + "_available"] = false; continue; }
        result[std::string(file) + "_available"] = true;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            if (!(fields >> key >> bytes)) continue;
            if (key == "Rss:" || key == "Pss:" || key == "Private_Clean:" ||
                key == "Private_Dirty:" || key == "VmRSS:" || key == "VmHWM:" || key == "VmSize:") {
                result[key.substr(0, key.size() - 1) + "_bytes"] = bytes * 1024;
            }
        }
    }
    return result;
}
class MemorySampler {
  public:
    explicit MemorySampler(fs::path cgroup) : cgroup_(std::move(cgroup)) {
        worker_ = std::thread([this] {
            while (!done_) {
                try {
                    peak_ = std::max(peak_.load(), read_number(cgroup_ / "memory.current"));
                    ++samples_;
                } catch (...) {
                    ++errors_;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~MemorySampler() { finish(); }
    json finish() {
        done_ = true;
        if (worker_.joinable()) worker_.join();
        return {{"sampled_cgroup_peak_bytes", peak_.load()}, {"samples", samples_.load()},
                {"read_errors", errors_.load()}, {"interval_ms", 10}};
    }
  private:
    fs::path cgroup_;
    std::atomic<bool> done_{false};
    std::atomic<uint64_t> peak_{0}, samples_{0}, errors_{0};
    std::thread worker_;
};
static json result_json(const Result &r) {
    return {{"exit_status", r.runtime_status}, {"timed_out", r.timed_out},
            {"output_limited", r.output_limited}, {"stdout", r.out}, {"stderr", r.err}};
}
static void require_success(const Result &r) {
    if (r.runtime_status != 0 || r.timed_out || r.output_limited)
        throw std::runtime_error("Guest command failed: " + r.err);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        std::cerr << "usage: sandbox-benchmark SOURCE ROOT minimal|host-tools HOST_MIB RUNS OUTPUT.json\n";
        return 2;
    }
    std::string active_id;
    std::unique_ptr<Sandbox> manager;
    try {
        const fs::path source = fs::canonical(argv[1]), root = argv[2];
        const std::string mode = argv[3];
        if (mode != "minimal" && mode != "host-tools") throw std::runtime_error("invalid mode");
        const unsigned budget = std::stoul(argv[4]), count = std::stoul(argv[5]);
        if (count < 2 || count > 1000) throw std::runtime_error("RUNS must be 2..1000");
        Options options;
        options.src_repo = source;
        options.environment = mode == "minimal" ? Environment::Minimal : Environment::HostTools;
        options.memory_bytes = size_t(budget) * 1024 * 1024;
        options.cpu_quota_us = 0;
        options.cmd_timeout = std::chrono::seconds(30);
        json report{{"mode", mode}, {"host_budget_mib", budget}, {"guest_ram_mib", budget / 2},
                    {"vcpus", 1}, {"cpu_quota_us", 0}, {"idle_settle_ms", 1000},
                    {"source_disk", disk_usage(source)}, {"runs", json::array()}};
        CrunWorkerClient guard(root / "runtime", true);
        for (unsigned index = 0; index < count; ++index) {
            std::cerr << mode << " " << budget << " MiB sample " << index + 1 << "/" << count << "\n";
            auto runtime = std::make_unique<MeasuredRuntime>();
            auto *rt = runtime.get();
            auto workspace = std::make_unique<MeasuredWorkspace>();
            auto *ws = workspace.get();
            manager = std::make_unique<Sandbox>(root, std::move(runtime), std::move(workspace));
            auto begin = Clock::now();
            const auto s = manager->create(options);
            const double create_ms = elapsed(begin);
            active_id = s.id;
            json run{{"index", index}, {"sandbox_id", s.id}, {"create_ms", create_ms},
                     {"workspace_ms", ws->create_ms}, {"prepare_ms", rt->prepare_ms},
                     {"runtime_start_ms", rt->start_ms}, {"resource_limits_verified", s.resource_limits_verified}};
            const auto state_result = guard.state(s.runtime_id);
            require_success(state_result);
            const int pid = json::parse(state_result.out).at("pid");
            const auto cgroup = cgroup_of(pid);
            run["vmm_pid"] = pid;
            run["cgroup"] = cgroup.string();
            run["memory_ready"] = memory_snapshot(pid, cgroup);
            run["disk_ready"] = sandbox_disk(s);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            run["memory_idle"] = memory_snapshot(pid, cgroup);
            run["true_exec_ms"] = json::array();
            for (unsigned n = 0; n < 6; ++n) {
                begin = Clock::now();
                const auto result = manager->execute( {"/bin/true"});
                const auto ms = elapsed(begin);
                require_success(result);
                run["true_exec_ms"].push_back(ms);
            }
            const auto meminfo = manager->execute( {"/bin/cat", "/proc/meminfo"});
            require_success(meminfo);
            run["guest_meminfo"] = meminfo.out;
            if (mode == "host-tools") {
                MemorySampler sampler(cgroup);
                begin = Clock::now();
                const auto compilation = manager->execute(
                    {"/usr/bin/g++", "-O2", "/workspace/main.cpp", "-o", "/build/app"});
                run["compile_ms"] = elapsed(begin);
                run["compile_result"] = result_json(compilation);
                run["compile_memory"] = sampler.finish();
                // An OOM is recorded as an outcome rather than discarded as an outlier.
                if (compilation.runtime_status == 0 && !compilation.timed_out && !compilation.output_limited) {
                    const auto execution = manager->execute( {"/build/app"});
                    require_success(execution);
                    if (execution.out != "49995000\n") throw std::runtime_error("unexpected workload result");
                    run["app_result"] = result_json(execution);
                }
            }
            run["memory_after_work"] = memory_snapshot(pid, cgroup);
            run["memory_events"] = read_text(cgroup / "memory.events");
            run["disk_after_work"] = sandbox_disk(s);
            begin = Clock::now();
            manager->stop();
            run["stop_ms"] = elapsed(begin);
            run["disk_stopped"] = sandbox_disk(s);
            begin = Clock::now();
            manager->destroy();
            run["destroy_ms"] = elapsed(begin);
            active_id.clear();
            // Give systemd time to remove an empty scope; never remove cgroups ourselves.
            for (unsigned n = 0; n < 100 && fs::exists(cgroup); ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            run["cleanup"] = {{"sandbox_removed", !fs::exists(s.directory)},
                              {"runtime_state_removed", !fs::exists(root / "runtime" / s.runtime_id)},
                              {"cgroup_removed", !fs::exists(cgroup)}};
            for (const auto &value : run["cleanup"].items())
                if (!value.value().get<bool>()) throw std::runtime_error("cleanup verification failed");
            report["runs"].push_back(run);
            // Preserve every completed sample even if a later run fails.
            std::ofstream out(argv[6]);
            out << report.dump(2) << '\n';
            if (!out) throw std::runtime_error("cannot write benchmark result");
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "benchmark failed: " << e.what() << '\n';
        if (manager && !active_id.empty()) {
            try { manager->destroy(); }
            catch (const std::exception &cleanup) { std::cerr << "cleanup failed: " << cleanup.what() << '\n'; }
        }
        return 1;
    }
}
