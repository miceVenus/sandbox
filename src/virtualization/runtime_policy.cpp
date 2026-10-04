#include "virtualization/runtime_policy.hpp"
#include "lib.hpp"
#include <fstream>
#include <unistd.h>
using json = nlohmann::json;
namespace fs = std::filesystem;

void check_rootless_cgroups(const Options &options) {
    require(fs::exists("/sys/fs/cgroup/cgroup.controllers"),
            "rootless resource limits require cgroup v2");
    const auto user_cgroup = fs::path("/sys/fs/cgroup/user.slice") /
                             ("user-" + std::to_string(geteuid()) + ".slice") /
                             ("user@" + std::to_string(geteuid()) + ".service");
    // Standard systemd login layout: diagnose missing delegation before creating
    // a info. Other layouts are checked against the actual container below.
    std::ifstream controllers(user_cgroup / "cgroup.controllers");
    if (controllers) {
        std::string content((std::istreambuf_iterator<char>(controllers)), {});
        require(content.find("memory") != std::string::npos &&
                    content.find("pids") != std::string::npos,
                "systemd user info must delegate memory and pids controllers");
        require(options.cpu_quota_us == 0 || content.find("cpu") != std::string::npos,
                "CPU controller is not delegated to your systemd user info; delegate cpu or "
                "explicitly use Options::cpu_quota_us = 0 for debugging (no CPU quota)");
    }
}
void verify_runtime_resources(const SandboxInfo &s, const json &state) {
    const auto pid = state.at("pid").get<int>();
    require(pid > 0, "crun did not return a container PID");
    std::ifstream groups(fs::path("/proc") / std::to_string(pid) / "cgroup");
    std::string line, path;
    while (std::getline(groups, line)) {
        if (line.rfind("0::/", 0) == 0) {
            path = line.substr(3);
        }
    }
    require(!path.empty() && path.find("bbm-sandbox-" + s.id) != std::string::npos,
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
                "Options::cpu_quota_us = 0 for debugging");
    }
}

