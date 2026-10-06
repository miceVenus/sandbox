#include "virtualization/container/crun_worker_client.hpp"
#include "resources.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
    // A sealed, anonymous request avoids argv encoding, filesystem artifacts,
    // and sharing task stdin with runtime control. Its owner outlives run_process.
    class ControlRequest {
      public:
        explicit ControlRequest(const std::vector<std::uint8_t> &bytes) {
            if (bytes.empty() || bytes.size() > crun_worker::max_request_bytes) {
                throw std::runtime_error("libcrun control request exceeds limit");
            }
            const int original = memfd_create("sandbox-crun-request", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (original < 0) {
                throw std::runtime_error("cannot allocate libcrun control request");
            }
            fd_ = fcntl(original, F_DUPFD_CLOEXEC, crun_worker::control_fd);
            close(original);
            if (fd_ < 0) {
                throw std::runtime_error("cannot duplicate libcrun control descriptor");
            }
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto written = write(fd_, bytes.data() + offset, bytes.size() - offset);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written <= 0) {
                    close(fd_);
                    throw std::runtime_error("cannot write libcrun control request");
                }
                offset += static_cast<std::size_t>(written);
            }
            if (lseek(fd_, 0, SEEK_SET) < 0 ||
                fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
                close(fd_);
                throw std::runtime_error("cannot seal libcrun control request");
            }
        }

        ~ControlRequest() { close(fd_); }
        ControlRequest(const ControlRequest &) = delete;
        auto operator=(const ControlRequest &) -> ControlRequest & = delete;
        [[nodiscard]] auto fd() const -> int { return fd_; }

      private:
        int fd_ = -1;
    };
} // namespace

auto CrunWorkerClient::call(
    const crun_worker::Request &request,
    int timeout_ms,
    bool drain_until_eof,
    size_t output_limit,
    int listener_fd) -> Result 
{
    using nlohmann::json;
    json message{{"version", crun_worker::protocol_version},
                 {"operation", static_cast<int>(request.operation)},
                 {"state_root", root_.string()},
                 {"id", request.id},
                 {"bundle", request.bundle},
                 {"agent_listener", listener_fd >= 0},
                 {"systemd_cgroups", systemd_cgroups_}};
    if (systemd_cgroups_) {
        // Host connection settings are private control data, never OCI process.env.
        const char *configured = std::getenv("XDG_RUNTIME_DIR");
        auto runtime = configured ? std::filesystem::path(configured)
                                  : std::filesystem::path("/run/user") / std::to_string(geteuid());
        struct stat st{};
        if (!runtime.is_absolute() || lstat(runtime.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 0077)) {
            throw std::runtime_error("rootless crun requires an owned, private XDG_RUNTIME_DIR and "
                                     "a systemd user session");
        }
        const auto *account = getpwuid(geteuid());
        if (!account) {
            throw std::runtime_error("cannot determine runtime user home");
        }
        message["host_environment"] = {{"runtime_directory", runtime.string()},
                                        {"home", account->pw_dir}};
    }
    const ControlRequest control(json::to_cbor(message));
    ProcessSupervision supervision;
    supervision.control_fd = control.fd();
    supervision.listener_fd = listener_fd;
    return run_process({sandbox_resources::runtime_runner_path().string()},
                       timeout_ms, output_limit, drain_until_eof, {}, supervision);
}

auto CrunWorkerClient::start(
    const std::string &id, 
    const std::string &bundle, 
    int listener_fd) -> Result 
{
    crun_worker::Request request(SANDBOX_CRUN_START, id);
    request.bundle = bundle;
    // Detached PID 1 may retain the pipes; do not wait for its lifetime.
    return call(request, 10000, false, 1024 * 1024, listener_fd);
}

auto CrunWorkerClient::state(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_STATE, id));
}

auto CrunWorkerClient::destroy(const std::string &id) -> bool {
    const auto killed = call(crun_worker::Request(SANDBOX_CRUN_KILL, id));
    const auto deleted = call(crun_worker::Request(SANDBOX_CRUN_DELETE, id));
    const bool ok = deleted.runtime_status == 0 && !deleted.timed_out && !deleted.output_limited;
    if (!ok) {
        std::cerr << "cleanup failed; inspect container manually:\n" << killed.err << deleted.err;
    }
    return ok;
}

auto CrunWorkerClient::pause(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_PAUSE, id));
}

auto CrunWorkerClient::resume(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_RESUME, id));
}
