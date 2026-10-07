#pragma once

#include "lib/descriptor.hpp"
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lib {
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;

    struct IoError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct IoTimeout : IoError {
        IoTimeout() : IoError("I/O deadline exceeded") {
        }
    };
    enum class DescriptorKind { Socket, Stream };

    void wait_fd(int descriptor, short events, Deadline deadline);
    class DescriptorStream {
      public:
        DescriptorStream(UniqueFd descriptor, DescriptorKind kind);
        void write_all(std::string_view bytes, Deadline deadline);
        auto read_exact(size_t size, Deadline deadline) -> std::string;
        void interrupt() noexcept;

      private:
        UniqueFd descriptor_;
        DescriptorKind kind_;
    };
} // namespace lib
