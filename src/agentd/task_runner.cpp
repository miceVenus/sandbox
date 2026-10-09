// Runs only inside the Guest, as a privileged child of the control service.
// Join the task cgroup before dropping identity and executing task code.
#include "agentd/task_runner.hpp"
#include "lib/descriptor.hpp"

#include <cstdio>
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
    const lib::UniqueFd group(open((std::string(task_cgroup) + "/cgroup.procs").c_str(),
                                  O_WRONLY | O_CLOEXEC));
    const auto pid = std::to_string(getpid());
    if (group.get() < 0 || write(group.get(), pid.data(), pid.size()) != ssize_t(pid.size())) {
        std::perror("join Guest task cgroup");
        return 126;
    }
    const rlimit nofile{256, 256}, core{0, 0};
    if (setrlimit(RLIMIT_NOFILE, &nofile) || setrlimit(RLIMIT_CORE, &core) ||
        setgroups(0, nullptr) || setgid(task_gid) || setuid(task_uid) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        std::perror("drop Guest task privileges");
        return 126;
    }
    execv(argv[1], argv + 1);
    std::perror("Guest task exec");
    return 127;
}
