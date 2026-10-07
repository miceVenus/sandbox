#pragma once

#include "lib/descriptor.hpp"
#include "lib/io.hpp"
#include <cstdint>
#include <filesystem>
#include <sys/socket.h>

namespace lib {
    // The caller owns the pathname lifecycle; no existing socket is unlinked.
    auto listen_unix(const std::filesystem::path &path, int backlog = 4) -> UniqueFd;
    auto listen_vsock(uint32_t port, int backlog = 4) -> UniqueFd;
    auto accept_socket(int listener, sockaddr *address = nullptr, socklen_t *size = nullptr)
        -> UniqueFd;
    auto connect_unix(const std::filesystem::path &path, Deadline deadline) -> UniqueFd;
} // namespace lib
