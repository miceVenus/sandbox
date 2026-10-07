#include "lib/socket.hpp"
#include "lib/error.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace lib {
    namespace {
        auto address_for(const std::filesystem::path &path) -> sockaddr_un {
            const auto name = path.string();
            sockaddr_un address{};
            require(!name.empty() && name.find('\0') == std::string::npos &&
                        name.size() < sizeof(address.sun_path),
                    "invalid Unix socket path");
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
            return address;
        }
        auto open_socket() -> UniqueFd {
            UniqueFd descriptor(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
            if (descriptor.get() < 0) {
                throw IoError(std::string("create Unix socket: ") + std::strerror(errno));
            }
            return descriptor;
        }
    } // namespace
    auto listen_unix(const std::filesystem::path &path, int backlog) -> UniqueFd {
        const auto address = address_for(path);
        auto descriptor = open_socket();
        if (bind(descriptor.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) <
                0 ||
            listen(descriptor.get(), backlog) < 0) {
            throw IoError(std::string("bind/listen Unix socket: ") + std::strerror(errno));
        }
        return descriptor;
    }
    auto listen_vsock(uint32_t port, int backlog) -> UniqueFd {
        UniqueFd descriptor(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (descriptor.get() < 0) {
            throw IoError(std::string("create vsock listener: ") + std::strerror(errno));
        }
        sockaddr_vm address{};
        address.svm_family = AF_VSOCK;
        address.svm_cid = VMADDR_CID_ANY;
        address.svm_port = port;
        if (bind(descriptor.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) <
                0 ||
            listen(descriptor.get(), backlog) < 0) {
            throw IoError(std::string("bind/listen vsock: ") + std::strerror(errno));
        }
        return descriptor;
    }

    auto accept_socket(int listener, sockaddr *address, socklen_t *size) -> UniqueFd {
        for (;;) {
            const int peer = accept4(listener, address, size, SOCK_CLOEXEC);
            if (peer >= 0) {
                return UniqueFd(peer);
            }
            if (errno != EINTR) {
                throw IoError(std::string("accept socket: ") + std::strerror(errno));
            }
        }
    }

    auto connect_unix(const std::filesystem::path &path, Deadline deadline) -> UniqueFd {
        const auto address = address_for(path);
        auto descriptor = open_socket();
        const auto flags = fcntl(descriptor.get(), F_GETFL);
        if (flags < 0 || fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
            throw IoError("configure Unix socket failed");
        }
        if (connect(descriptor.get(), reinterpret_cast<const sockaddr *>(&address),
                    sizeof(address)) < 0) {
            if (errno != EINPROGRESS) {
                throw IoError(std::string("connect Unix socket: ") + std::strerror(errno));
            }
            wait_fd(descriptor.get(), POLLOUT, deadline);
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                throw IoError("query Unix socket connection failed");
            }
            if (error != 0) {
                throw IoError(std::string("connect Unix socket: ") + std::strerror(error));
            }
        }
        return descriptor;
    }
} // namespace lib
