#pragma once

#include "lib/descriptor.hpp"
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ipc {
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;

    struct IoError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct IoTimeout : IoError {
        IoTimeout() : IoError("I/O deadline exceeded") {
        }
    };

    void wait_fd(int descriptor, short events, Deadline deadline);

    // Owns a connected socket; per-call nonblocking I/O enforces the deadline.
    class SocketStream {
      public:
        explicit SocketStream(lib::UniqueFd descriptor);
        void write_all(std::string_view bytes, Deadline deadline);
        auto read_exact(size_t size, Deadline deadline) -> std::string;
        void interrupt() noexcept;

      private:
        lib::UniqueFd descriptor_;
    };
} // namespace ipc
