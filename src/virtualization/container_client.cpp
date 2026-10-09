#include "virtualization/container_client.hpp"
#include "lib/error.hpp"
#include "lib/memory_file.hpp"
#include "lib/process.hpp"
#include "resources.hpp"
#include "virtualization/container/crun_request.hpp"

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

auto ContainerClient::call(const crun_worker::Request &request, int listener_fd) -> Result {
    using nlohmann::json;
    json message{{"version", crun_worker::protocol_version},
                 {"operation", static_cast<int>(request.operation)},
                 {"state_root", root_.string()},
                 {"id", request.id},
                 {"bundle", request.bundle},
                 {"agentd_listener", listener_fd >= 0},
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

    const auto bytes = json::to_cbor(message);
    lib::require(!bytes.empty() && bytes.size() <= crun_worker::max_request_bytes,
                 "libcrun control request exceeds limit");

    const auto control = lib::sealed_memory_file(
        "sandbox-crun-request",
        std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
        crun_worker::control_fd);
        
    lib::ProcessOptions options;
    // Detached PID 1 may retain these pipes after the startup worker exits.
    options.drain_until_eof = request.operation != SANDBOX_CRUN_START;
    options.inherited_fds.push_back(control.get());
    if (listener_fd >= 0) {
        options.inherited_fds.push_back(listener_fd);
    }
    return lib::run_process({sandbox_resources::crun_worker_path().string()}, options);
}

auto ContainerClient::start(const std::string &id, const std::string &bundle, int listener_fd)
    -> Result {
    crun_worker::Request request(SANDBOX_CRUN_START, id);
    request.bundle = bundle;
    return call(request, listener_fd);
}

auto ContainerClient::state(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_STATE, id));
}

auto ContainerClient::destroy(const std::string &id) -> bool {
    const auto killed = call(crun_worker::Request(SANDBOX_CRUN_KILL, id));
    const auto deleted = call(crun_worker::Request(SANDBOX_CRUN_DELETE, id));
    const bool ok = deleted.runtime_status == 0 && !deleted.timed_out && !deleted.output_limited;
    if (!ok) {
        std::cerr << "cleanup failed; inspect container manually:\n" << killed.err << deleted.err;
    }
    return ok;
}

auto ContainerClient::pause(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_PAUSE, id));
}

auto ContainerClient::resume(const std::string &id) -> Result {
    return call(crun_worker::Request(SANDBOX_CRUN_RESUME, id));
}
