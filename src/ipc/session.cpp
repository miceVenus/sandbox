#include "ipc/session.hpp"
#include "lib/error.hpp"
#include <utility>

namespace ipc {
    Session::Session(lib::UniqueFd descriptor) {
        attach(std::move(descriptor));
    }

    void Session::attach(lib::UniqueFd descriptor) {
        std::scoped_lock lock(mutex_);
        lib::require(!stream_, "session is already connected");
        stream_ = std::make_shared<SocketStream>(std::move(descriptor));
        valid_ = true;
    }

    auto Session::stream() const -> std::shared_ptr<SocketStream> {
        std::scoped_lock lock(mutex_);
        lib::require(stream_ != nullptr, "session is not connected");
        require(valid_, "session is unusable");
        return stream_;
    }

    auto Session::is_connected() const -> bool {
        std::scoped_lock lock(mutex_);
        return stream_ != nullptr;
    }

    void Session::disconnect() {
        std::shared_ptr<SocketStream> previous;
        {
            std::scoped_lock lock(mutex_);
            previous = std::move(stream_);
            valid_ = false;
        }
        if (previous) {
            previous->interrupt();
        }
    }

    void Session::invalidate() {
        std::scoped_lock lock(mutex_);
        if (stream_ && valid_) {
            valid_ = false;
            stream_->interrupt();
        }
    }

    void Session::invalidate(const std::shared_ptr<SocketStream> &stream) {
        std::scoped_lock lock(mutex_);
        // An operation on a disconnected stream must not invalidate a new connection.
        if (stream_ == stream && valid_) {
            valid_ = false;
            stream->interrupt();
        }
    }

    void Session::send(const Message &message, Deadline deadline) {
        const auto frame = encode(message);
        std::scoped_lock lock(writer_);
        const auto active = stream();
        try {
            active->write_all(frame, deadline);
        } catch (...) {
            invalidate(active);
            throw;
        }
    }

    auto Session::receive(Deadline deadline) -> Message {
        const auto active = stream();
        try {
            auto frame = active->read_exact(4, deadline);
            uint32_t length = 0;
            for (unsigned char byte : frame) {
                length = (length << 8) | byte;
            }
            require(length >= 5 && length <= max_frame_bytes, "invalid frame length");
            frame += active->read_exact(length, deadline);
            return decode(frame);
        } catch (...) {
            invalidate(active);
            throw;
        }
    }
} // namespace ipc
