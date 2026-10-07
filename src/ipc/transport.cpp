#include "ipc/transport.hpp"
#include "lib/descriptor.hpp"
#include "lib/socket.hpp"

namespace ipc {
    namespace {
        class DescriptorTransport final : public Transport {
          public:
            DescriptorTransport(int descriptor, DescriptorKind kind)
                : stream_(lib::UniqueFd(descriptor), kind == DescriptorKind::Socket
                                                         ? lib::DescriptorKind::Socket
                                                         : lib::DescriptorKind::Stream) {
            }
            void interrupt() noexcept override {
                stream_.interrupt();
            }
            void write_all(std::string_view bytes, Deadline deadline) override {
                stream_.write_all(bytes, deadline);
            }
            auto read_exact(size_t size, Deadline deadline) -> std::string override {
                require(size <= max_frame_bytes, "transport read exceeds frame limit");
                return stream_.read_exact(size, deadline);
            }

          private:
            lib::DescriptorStream stream_;
        };
    } // namespace
    auto adopt_descriptor(int descriptor, DescriptorKind kind) -> std::unique_ptr<Transport> {
        return std::make_unique<DescriptorTransport>(descriptor, kind);
    }
    auto connect_unix(const std::filesystem::path &path, Deadline deadline)
        -> std::unique_ptr<Transport> {
        auto descriptor = lib::connect_unix(path, deadline);
        return adopt_descriptor(descriptor.release(), DescriptorKind::Socket);
    }
    Channel::Channel(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {
        require(transport_ != nullptr, "agentd transport is required");
    }

    void Channel::invalidate() {
        if (valid_.exchange(false)) {
            transport_->interrupt();
        }
    }

    void Channel::send(const Message &message, Deadline deadline) {
        const auto frame = encode(message);
        std::scoped_lock lock(writer_);
        require(valid_, "agentd channel is unusable");
        try {
            transport_->write_all(frame, deadline);
        } catch (...) {
            invalidate();
            throw;
        }
    }

    auto Channel::receive(Deadline deadline) -> Message {
        require(valid_, "agentd channel is unusable");
        try {
            auto frame = transport_->read_exact(4, deadline);
            uint32_t length = 0;
            for (unsigned char byte : frame) {
                length = (length << 8) | byte;
            }
            require(length >= 5 && length <= max_frame_bytes, "invalid frame length");
            frame += transport_->read_exact(length, deadline);
            return decode(frame);
        } catch (...) {
            invalidate();
            throw;
        }
    }
} // namespace ipc
