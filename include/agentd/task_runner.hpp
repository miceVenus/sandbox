#pragma once

namespace agentd {
    // Internal agentd child mode: argv starts with "--", followed by the command.
    // Joins the fixed task cgroup and drops privileges before executing task code.
    auto run_task(int argc, char **argv) -> int;
} // namespace agentd
