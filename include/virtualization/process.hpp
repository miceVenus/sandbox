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
};

Result run_process(const std::vector<std::string> &args,
                   int timeout_ms,
                   size_t output_limit = 1024 * 1024,
                   bool drain_until_eof = true,
                   std::string_view stdin_data = {},
                   const ProcessSupervision &supervision = {});
