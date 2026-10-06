#pragma once

#include "../process.hpp"
#include "crun_worker_protocol.hpp"
#include <filesystem>

class CrunWorkerClient {
  public:
    // Internal transport to the SDK-owned libcrun worker. Not a public SDK API.
    explicit CrunWorkerClient(std::filesystem::path root, bool systemd_cgroups)
        : root_(std::move(root)), systemd_cgroups_(systemd_cgroups) {
    }

    auto start(const std::string &id, const std::string &bundle, int listener_fd = -1) -> Result;

    auto state(const std::string &id) -> Result;
    auto pause(const std::string &id) -> Result;
    auto resume(const std::string &id) -> Result;
    auto destroy(const std::string &id) -> bool;

  private:
    auto call(const crun_worker::Request &request,
                int timeout_ms = 10000,
                bool drain_until_eof = true,
                size_t output_limit = 1024 * 1024,
                int listener_fd = -1) -> Result;
    std::filesystem::path root_;
    bool systemd_cgroups_;
};
