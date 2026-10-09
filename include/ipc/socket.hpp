#pragma once

#include "ipc/io.hpp"
#include <cstdint>
#include <filesystem>
#include <sys/socket.h>

namespace ipc {
    // The caller owns the pathname lifecycle; no existing socket is unlinked.
    auto listen_unix(const std::filesystem::path &path, int backlog = 4) -> lib::UniqueFd;
    auto listen_vsock(uint32_t port, int backlog = 4) -> lib::UniqueFd;
    auto accept_socket(int listener, sockaddr *address = nullptr, socklen_t *size = nullptr)
        -> lib::UniqueFd;
    auto connect_unix(const std::filesystem::path &path, Deadline deadline) -> lib::UniqueFd;
} // namespace ipc
