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

namespace lib {

    struct ProcessOptions {
        int timeout_ms = 10000;
        size_t output_limit = 1024 * 1024;
        // Container startup workers can exit while a detached process retains the pipes.
        bool drain_until_eof = true;
        // Borrowed until run_process returns; empty connects stdin to /dev/null.
        std::string_view stdin_data;
        std::filesystem::path cwd;
        const std::atomic<bool> *cancel = nullptr;
        // Called with bounded output chunks; false denotes stdout, true denotes stderr.
        std::function<void(bool, std::string_view)> on_output;
        // Child processes must not leave children in their process group after exit.
        bool kill_remaining_group = false;
        // Caller-owned descriptors mapped consecutively to child FDs 3, 4, ... .
        // All other inherited descriptors are closed before execution.
        std::vector<int> inherited_fds;
        // Trusted launcher environment; empty keeps the fixed PATH/LANG defaults.
        std::vector<std::string> environment;
    };

    auto run_process(const std::vector<std::string> &args, const ProcessOptions &options = {})
        -> Result;

    void check_process_result(const Result &result);
} // namespace lib
