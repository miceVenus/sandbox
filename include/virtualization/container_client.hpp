#pragma once

#include "lib/process.hpp"
#include <filesystem>
#include <string>
#include <utility>

namespace crun_worker {
    struct Request;
}

// Shared container lifecycle client for the OCI runtime and the VMM's Host guard.
// sandbox tasks use the separate persistent agentd connection. This is an internal
// SDK component; the current implementation sends requests to a libcrun worker.
class ContainerClient {
  public:
    explicit ContainerClient(std::filesystem::path root, bool systemd_cgroups)
        : root_(std::move(root)), systemd_cgroups_(systemd_cgroups) {
    }

    auto start(const std::string &id, const std::string &bundle, int listener_fd = -1) -> Result;

    auto state(const std::string &id) -> Result;
    auto pause(const std::string &id) -> Result;
    auto resume(const std::string &id) -> Result;
    auto destroy(const std::string &id) -> bool;

  private:
    auto call(const crun_worker::Request &request, int listener_fd = -1) -> Result;
    std::filesystem::path root_;
    bool systemd_cgroups_;
};
