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

    Result start(const std::string &id, const std::string &bundle, int listener_fd = -1);

    Result state(const std::string &id);
    Result pause(const std::string &id);
    Result resume(const std::string &id);
    bool destroy(const std::string &id);

  private:
    Result call(const crun_worker::Request &request,
                int timeout_ms = 10000,
                bool drain_until_eof = true,
                size_t output_limit = 1024 * 1024,
                int listener_fd = -1);
    std::filesystem::path root_;
    bool systemd_cgroups_;
};
