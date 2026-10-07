// Runs only inside the Guest, as a privileged child of the control service.
// Join the task cgroup before dropping identity and executing task code.
#include "agentd/task_runner.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

auto agentd::run_task(int argc, char **argv) -> int {
    if (argc < 2 || geteuid() != 0 || std::strcmp(argv[0], "--") != 0) {
        std::fputs("invalid Guest task invocation\n", stderr);
        return 126;
    }
    const int group = open("/sys/fs/cgroup/sandbox-tasks/cgroup.procs", O_WRONLY | O_CLOEXEC);
    const auto pid = std::to_string(getpid());
    if (group < 0 || write(group, pid.data(), pid.size()) != ssize_t(pid.size())) {
        std::perror("join Guest task cgroup");
        return 126;
    }
    close(group);
    const rlimit nofile{256, 256}, core{0, 0};
    if (setrlimit(RLIMIT_NOFILE, &nofile) || setrlimit(RLIMIT_CORE, &core) ||
        setgroups(0, nullptr) || setgid(65534) || setuid(65534) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        std::perror("drop Guest task privileges");
        return 126;
    }
    const char *environment[][2] = {
        {"HOME", "/env/home"},           {"PATH", "/env/python/bin:/usr/bin:/bin"},
        {"TMPDIR", "/build/tmp"},        {"XDG_CACHE_HOME", "/cache"},
        {"PIP_CACHE_DIR", "/cache/pip"}, {"PIP_REQUIRE_VIRTUALENV", "true"},
        {"GIT_CONFIG_NOSYSTEM", "1"},    {"GIT_CONFIG_GLOBAL", "/dev/null"},
        {"GIT_TERMINAL_PROMPT", "0"}};
    for (const auto &entry : environment) {
        if (setenv(entry[0], entry[1], 1) != 0) {
            std::perror("configure Guest task environment");
            return 126;
        }
    }
    execv(argv[1], argv + 1);
    std::perror("Guest task exec");
    return 127;
}
