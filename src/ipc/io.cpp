#include "ipc/io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <utility>

namespace ipc {
    namespace {
        [[noreturn]] void io_error(const char *operation) {
            throw IoError(std::string(operation) + ": " + std::strerror(errno));
        }
    } // namespace
    void wait_fd(int fd, short events, Deadline deadline) {
        for (;;) {
            if (Clock::now() >= deadline) {
                throw IoTimeout();
            }
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                    .count();
            pollfd item{fd, events, 0};
            const auto result = poll(&item, 1, static_cast<int>(std::clamp<int64_t>(ms, 1, 1000)));
            if (result > 0) {
                if (item.revents & POLLNVAL) {
                    throw IoError("invalid descriptor");
                }
                return;
            }
            if (result < 0 && errno != EINTR) {
                io_error("poll descriptor");
            }
        }
    }

    SocketStream::SocketStream(lib::UniqueFd descriptor)
        : descriptor_(std::move(descriptor)) {
        const int fd = descriptor_.get();
        if (fd < 0) {
            throw IoError("invalid descriptor");
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            io_error("configure descriptor");
        }
    }

    void SocketStream::interrupt() noexcept {
        shutdown(descriptor_.get(), SHUT_RDWR);
    }

    void SocketStream::write_all(std::string_view bytes, Deadline deadline) {
        while (!bytes.empty()) {
            if (Clock::now() >= deadline) {
                throw IoTimeout();
            }
            const auto count = send(descriptor_.get(), bytes.data(), bytes.size(),
                                    MSG_NOSIGNAL | MSG_DONTWAIT);
            if (count > 0) {
                bytes.remove_prefix(static_cast<size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                wait_fd(descriptor_.get(), POLLOUT, deadline);
            } else {
                io_error("write bytes");
            }
        }
    }

    auto SocketStream::read_exact(size_t size, Deadline deadline) -> std::string {
        std::string output(size, '\0');
        size_t offset = 0;
        while (offset < size) {
            if (Clock::now() >= deadline) {
                throw IoTimeout();
            }
            const auto count =
                recv(descriptor_.get(), output.data() + offset, size - offset, MSG_DONTWAIT);
            if (count > 0) {
                offset += static_cast<size_t>(count);
            } else if (count == 0) {
                throw IoError("descriptor closed before completing read");
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_fd(descriptor_.get(), POLLIN, deadline);
            } else {
                io_error("read bytes");
            }
        }
        return output;
    }

} // namespace ipc
