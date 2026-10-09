#pragma once

#include <sys/types.h>

namespace agentd {
    inline constexpr uid_t task_uid = 65534;
    inline constexpr gid_t task_gid = 65534;
    inline constexpr const char *task_cgroup = "/sys/fs/cgroup/sandbox-tasks";
    // Internal agentd child mode: argv starts with "--", followed by the command.
    // Joins the fixed task cgroup and drops privileges before executing task code.
    auto run_task(int argc, char **argv) -> int;
} // namespace agentd
