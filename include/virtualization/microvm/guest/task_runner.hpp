#pragma once

namespace protocol {
    // Internal agentd child mode: argv starts with "--", followed by the command.
    // Joins the fixed task cgroup and drops privileges before executing Agent code.
    int run_guest_task(int argc, char **argv);
}
