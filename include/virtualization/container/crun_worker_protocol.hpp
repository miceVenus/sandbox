#pragma once

#include "libcrun_operations.h"
#include <cstddef>
#include <string>
#include <utility>

namespace crun_worker {
    inline constexpr int control_fd = 3;
    inline constexpr unsigned protocol_version = 2;
    inline constexpr std::size_t max_request_bytes = 1024 * 1024;

    struct Request {
        Request(sandbox_crun_operation operation, std::string id)
            : operation(operation), id(std::move(id)) {}

        sandbox_crun_operation operation;
        std::string id;
        std::string bundle;

    };
} // namespace crun_worker
