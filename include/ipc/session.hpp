#pragma once

#include "ipc/protocol.hpp"
#include "ipc/io.hpp"
#include <memory>
#include <mutex>

namespace ipc {
    // Framed communication over an established socket; no business-level handshake.
    // One reader may run alongside multiple senders.
    class Session {
      public:
        Session() = default;
        explicit Session(lib::UniqueFd descriptor);
        void attach(lib::UniqueFd descriptor);
        [[nodiscard]] auto is_connected() const -> bool;
        void disconnect();
        // A partial frame or timeout invalidates the session; operations are never replayed.
        void invalidate();
        void send(const Message &message, Deadline deadline);
        auto receive(Deadline deadline) -> Message;

      private:
        auto stream() const -> std::shared_ptr<SocketStream>;
        void invalidate(const std::shared_ptr<SocketStream> &stream);
        mutable std::mutex mutex_;
        std::shared_ptr<SocketStream> stream_;
        std::mutex writer_;
        bool valid_ = false;
    };
} // namespace ipc
