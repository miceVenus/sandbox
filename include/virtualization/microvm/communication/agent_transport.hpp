#pragma once

#include "agent_protocol.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>

namespace protocol {
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;

    struct TransportError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct Timeout : TransportError {
        Timeout() : TransportError("agent transport deadline exceeded") {
        }
    };

    // One reader and one writer may operate concurrently. Implementations own their resource.
    class Transport {
      public:
        virtual ~Transport() = default;
        virtual void write_all(std::string_view bytes, Deadline deadline) = 0;
        virtual std::string read_exact(size_t size, Deadline deadline) = 0;
        virtual void interrupt() noexcept {
        }
    };

    // Takes ownership even on failure. SerialPort requires a dedicated virtio port,
    // not a boot log or terminal channel. Socket uses MSG_NOSIGNAL.
    enum class DescriptorKind { Socket, SerialPort };
    std::unique_ptr<Transport> adopt_descriptor(int fd, DescriptorKind kind);
    std::unique_ptr<Transport> connect_unix(const std::filesystem::path &path, Deadline deadline);

    class Channel {
      public:
        explicit Channel(std::unique_ptr<Transport> transport);
        void send(const Message &message, Deadline deadline);
        Message receive(Deadline deadline);
        // A partial frame/timeout makes this channel unusable; it is never retried.
        void invalidate();

      private:
        std::unique_ptr<Transport> transport_;
        std::mutex writer_;
        std::atomic<bool> valid_{true};
    };
} // namespace protocol
