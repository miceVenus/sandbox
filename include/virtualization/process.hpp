#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct Result {
    int runtime_status = -1; // Worker/task exit status; diagnostics remain in err.
    bool timed_out = false;
    bool output_limited = false;
    std::string out, err;
    bool cancelled = false;
};

struct ProcessSupervision {
    std::filesystem::path cwd;
    const std::atomic<bool> *cancel = nullptr;
    // Called with bounded output chunks; false denotes stdout, true denotes stderr.
    std::function<void(bool, std::string_view)> on_output;
    // Guest tasks must not leave children in their process group after exit.
    bool kill_remaining_group = false;
    // Optional caller-owned control descriptor (>= 3), mapped to child FD 3.
    // stdin/stdout/stderr remain task streams. The worker must consume and close
    // this descriptor before starting untrusted code; all other FDs are closed.
    int control_fd = -1;
    // A host-created listener passed only to a runtime worker as child FD 4.
    int listener_fd = -1;
    // Trusted launcher environment; empty keeps the fixed PATH/LANG defaults.
    std::vector<std::string> environment;
};

Result run_process(const std::vector<std::string> &args,
                   int timeout_ms,
                   size_t output_limit = 1024 * 1024,
                   bool drain_until_eof = true,
                   std::string_view stdin_data = {},
                   const ProcessSupervision &supervision = {});
