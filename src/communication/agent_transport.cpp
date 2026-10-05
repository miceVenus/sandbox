#include "communication/agent_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace protocol {
    namespace {
        [[noreturn]] void io_error(const char *operation) {
            throw TransportError(std::string(operation) + ": " + std::strerror(errno));
        }

        void wait_fd(int fd, short events, Deadline deadline) {
            for (;;) {
                if (Clock::now() >= deadline) {
                    throw Timeout();
                }
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                        .count();
                pollfd item{fd, events, 0};
                const auto result =
                    poll(&item, 1, static_cast<int>(std::clamp<int64_t>(ms, 1, 1000)));
                if (result > 0) {
                    if (item.revents & POLLNVAL) {
                        throw TransportError("invalid agent descriptor");
                    }
                    return;
                }
                if (result < 0 && errno != EINTR) {
                    io_error("poll agent descriptor");
                }
            }
        }

        class DescriptorTransport final : public Transport {
          public:
            DescriptorTransport(int fd, DescriptorKind kind) : fd_(fd), kind_(kind) {
                if (fd < 0) {
                    throw TransportError("invalid agent descriptor");
                }
                const auto flags = fcntl(fd, F_GETFL);
                if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
                    fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
                    const auto error = errno;
                    close(fd);
                    errno = error;
                    io_error("configure agent descriptor");
                }
            }

            ~DescriptorTransport() override {
                close(fd_);
            }

            void interrupt() noexcept override {
                if (kind_ == DescriptorKind::Socket) {
                    shutdown(fd_, SHUT_RDWR);
                }
            }

            void write_all(std::string_view bytes, Deadline deadline) override {
                while (!bytes.empty()) {
                    if (Clock::now() >= deadline) {
                        throw Timeout();
                    }
                    const auto count = kind_ == DescriptorKind::Socket
                                           ? send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL)
                                           : ::write(fd_, bytes.data(), bytes.size());
                    if (count > 0) {
                        bytes.remove_prefix(static_cast<size_t>(count));
                    } else if (count < 0 && errno == EINTR) {
                        continue;
                    } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        wait_fd(fd_, POLLOUT, deadline);
                    } else {
                        io_error("write agent frame");
                    }
                }
            }

            std::string read_exact(size_t size, Deadline deadline) override {
                require(size <= max_frame_bytes, "transport read exceeds frame limit");
                std::string output(size, '\0');
                size_t offset = 0;
                while (offset < size) {
                    if (Clock::now() >= deadline) {
                        throw Timeout();
                    }
                    const auto count = ::read(fd_, output.data() + offset, size - offset);
                    if (count > 0) {
                        offset += static_cast<size_t>(count);
                    } else if (count == 0) {
                        throw TransportError("agent connection closed mid-frame");
                    } else if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        wait_fd(fd_, POLLIN, deadline);
                    } else {
                        io_error("read agent frame");
                    }
                }
                return output;
            }

          private:
            int fd_;
            DescriptorKind kind_;
        };
    } // namespace

    std::unique_ptr<Transport> adopt_descriptor(int fd, DescriptorKind kind) {
        return std::make_unique<DescriptorTransport>(fd, kind);
    }

    std::unique_ptr<Transport> connect_unix(const std::filesystem::path &path, Deadline deadline) {
        const auto name = path.string();
        sockaddr_un address{};
        require(!name.empty() && name.find('\0') == std::string::npos &&
                    name.size() < sizeof(address.sun_path),
                "invalid agent Unix socket path");
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        auto transport = adopt_descriptor(fd, DescriptorKind::Socket);
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
            // AF_UNIX EAGAIN means backlog full, not an asynchronous successful connection.
            if (errno != EINPROGRESS) {
                io_error("connect agent Unix socket");
            }
            wait_fd(fd, POLLOUT, deadline);
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                io_error("query agent connection");
            }
            if (error != 0) {
                errno = error;
                io_error("connect agent Unix socket");
            }
        }
        return transport;
    }

    Channel::Channel(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {
        require(transport_ != nullptr, "agent transport is required");
    }

    void Channel::invalidate() {
        if (valid_.exchange(false)) {
            transport_->interrupt();
        }
    }

    void Channel::send(const Message &message, Deadline deadline) {
        const auto frame = encode(message);
        std::lock_guard<std::mutex> lock(writer_);
        require(valid_, "agent channel is unusable");
        try {
            transport_->write_all(frame, deadline);
        } catch (...) {
            invalidate();
            throw;
        }
    }

    Message Channel::receive(Deadline deadline) {
        require(valid_, "agent channel is unusable");
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
} // namespace protocol
