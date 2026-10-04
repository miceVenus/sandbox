#pragma once

#include "../process.hpp"
#include <filesystem>

class CrunWorkerClient {
  public:
    // Internal transport to the SDK-owned libcrun worker. Not a public SDK API.
    explicit CrunWorkerClient(std::filesystem::path root, bool systemd_cgroups)
        : root_(std::move(root)), systemd_cgroups_(systemd_cgroups) {
    }

    Result start(const std::string &id, const std::string &bundle);

    Result exec(const std::string &id,
                const std::string &cwd,
                const std::vector<std::string> &argv,
                int ms,
                size_t output_limit = 1024 * 1024,
                std::string_view stdin_data = {});

    Result state(const std::string &id);
    Result pause(const std::string &id);
    Result resume(const std::string &id);
    bool destroy(const std::string &id);

  private:
    Result call(const std::vector<std::string> &options,
                int timeout_ms = 10000,
                bool drain_until_eof = true,
                size_t output_limit = 1024 * 1024,
                std::string_view stdin_data = {});
    std::filesystem::path root_;
    bool systemd_cgroups_;
};
