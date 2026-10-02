#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct Result {
    int runtime_status = -1; // crun status, not an independently verified task status
    bool timed_out = false;
    bool output_limited = false;
    std::string out, err;
};

Result run_process(const std::vector<std::string> &args,
                   int timeout_ms,
                   size_t output_limit = 1024 * 1024,
                   bool drain_until_eof = true,
                   std::string_view stdin_data = {});
