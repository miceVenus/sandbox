#include "virtualization/oci.hpp"
#include "lib.hpp"
#include "sandbox.hpp"
#include "virtualization/host_tools.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

using json = nlohmann::json;

void prepare_oci_config(const SandboxInfo &s) {

    const auto &o = s.options;

    const bool rootless = geteuid() != 0;

    json mounts = json::array();
    auto mount = [&](std::string destination, std::string type, std::string source, json options) {
        mounts.push_back({{"destination", destination},
                          {"type", type},
                          {"source", source},
                          {"options", options}});
    };
    if (o.environment == Environment::HostTools) {
        for (const auto &tools : host_tool_mounts()) {
            // Nonrecursive bind excludes nested host mounts. Never use rbind.
            mount(tools.destination.string(),
                  "bind",
                  tools.source.string(),
                  {"bind", "ro", "nosuid", "nodev", "private"});
        }
        for (const auto &directory : {"build", "env", "cache"}) {
            mount(std::string("/") + directory,
                  "bind",
                  (s.directory / "runtime-data" / directory).string(),
                  {"bind", "rw", "nosuid", "nodev", "private"});
        }
    }
    mount("/proc", "proc", "proc", {"nosuid", "nodev", "noexec"});
    mount("/dev", "tmpfs", "tmpfs", {"nosuid", "strictatime", "mode=755", "size=65536k"});
    mount("/dev/pts",
          "devpts",
          "devpts",
          {"nosuid",
           "noexec",
           "newinstance",
           "ptmxmode=0666",
           "mode=0620",
           rootless ? "gid=0" : "gid=5"});
    mount("/dev/shm", "tmpfs", "shm", {"nosuid", "nodev", "noexec", "mode=1777", "size=16m"});
    mount("/tmp", "tmpfs", "tmpfs", {"nosuid", "nodev", "noexec", "mode=1777", "size=16m"});
    mount(o.ctr_repo.string(),
          "bind",
          s.work_files_dir.string(),
          {"bind", "rw", "nosuid", "nodev", "private"});
    json namespaces = json::array();
    for (const auto &type : {"pid", "network", "ipc", "uts", "cgroup", "mount"}) {
        namespaces.push_back({{"type", type}});
    }
    json devices = json::array({{{"allow", false}, {"access", "rwm"}}});
    for (int minor : {3, 5, 7, 8, 9}) {
        devices.push_back(
            {{"allow", true}, {"type", "c"}, {"major", 1}, {"minor", minor}, {"access", "rw"}});
    }
    devices.push_back(
        {{"allow", true}, {"type", "c"}, {"major", 5}, {"minor", 0}, {"access", "rw"}});

    json config = {
        {"ociVersion", "1.0.0"},
        {"hostname", "agent-sandbox"},
        {"root", {{"path", "rootfs"}, {"readonly", true}}},
        {"mounts", mounts},
        {"process",
         {{"terminal", false},
          {"user", {{"uid", 65534}, {"gid", 65534}, {"additionalGids", json::array()}}},
          {"args", {"/bin/sh", "-c", "while :; do /bin/sleep 3600; done"}},
          {"cwd", get_cwd(o.ctr_repo, o.cwd_rlt).string()},
          {"env",
           {"PATH=/bin:/usr/bin",
            "HOME=" + o.ctr_repo.string(),
            "LANG=C",
            "GIT_CONFIG_NOSYSTEM=1",
            "GIT_CONFIG_GLOBAL=/dev/null",
            "GIT_TERMINAL_PROMPT=0"}},
          {"noNewPrivileges", true},
          {"capabilities",
           {{"bounding", json::array()},
            {"effective", json::array()},
            {"permitted", json::array()},
            {"inheritable", json::array()},
            {"ambient", json::array()}}},
          {"rlimits",
           json::array({{{"type", "RLIMIT_NOFILE"}, {"soft", 256}, {"hard", 256}},
                        {{"type", "RLIMIT_CORE"}, {"soft", 0}, {"hard", 0}}})}}},
        {"linux",
         {{"namespaces", namespaces},
          {"cgroupsPath", "/bbm-sandbox-" + s.id},
          {"resources",
           {{"devices", devices},
            {"memory", {{"limit", o.memory_bytes}, {"swap", o.memory_bytes}}},
            {"cpu", {{"period", o.cpu_period_us}, {"quota", o.cpu_quota_us}}},
            {"pids", {{"limit", o.max_tasks}}}}},
          {"maskedPaths",
           {"/proc/kcore",
            "/proc/keys",
            "/proc/timer_list",
            "/proc/latency_stats",
            "/proc/sched_debug",
            "/sys/firmware"}},
          {"readonlyPaths",
           {"/proc/bus", "/proc/fs", "/proc/irq", "/proc/sys", "/proc/sysrq-trigger"}}}}};

    if (o.environment == Environment::HostTools) {
        config["process"]["env"] = {"PATH=/env/python/bin:/usr/bin:/bin",
                                    "HOME=/env/home",
                                    "LANG=C",
                                    "TMPDIR=/build/tmp",
                                    "XDG_CACHE_HOME=/cache",
                                    "PIP_CACHE_DIR=/cache/pip",
                                    "PIP_REQUIRE_VIRTUALENV=true",
                                    "GIT_CONFIG_NOSYSTEM=1",
                                    "GIT_CONFIG_GLOBAL=/dev/null",
                                    "GIT_TERMINAL_PROMPT=0"};
    }

    if (rootless) {
        // Single-ID mapping keeps workspace ownership with the invoking user;
        // container UID 0 has no host-root identity or Linux capabilities.
        config["process"]["user"]["uid"] = 0;
        config["process"]["user"]["gid"] = 0;
        config["linux"]["namespaces"].push_back({{"type", "user"}});
        config["linux"]["uidMappings"] =
            json::array({{{"containerID", 0}, {"hostID", geteuid()}, {"size", 1}}});
        config["linux"]["gidMappings"] =
            json::array({{{"containerID", 0}, {"hostID", getegid()}, {"size", 1}}});
        config["linux"]["cgroupsPath"] = "user.slice:bbm-sandbox:" + s.id;
        // An unprivileged runtime cannot install a device eBPF filter. Only
        // standard /dev entries are provided; writable workspace is nodev and
        // the task has no CAP_MKNOD. There is no host /dev bind mount.
        config["linux"]["resources"].erase("devices");
    }

    if (o.cpu_quota_us == 0) {
        config["linux"]["resources"].erase("cpu");
    }
    std::ofstream out(s.bundle_dir / "config.json");
    out << config.dump(2);
    out.close();
    require(bool(out), "cannot write OCI config");
}
