// SDK-internal service, deployed as container PID 1 or inside a Guest.
#include "agentd/container_bootstrap.hpp"
#include "agentd/microvm_bootstrap.hpp"
#include "agentd/service.hpp"
#include "agentd/task_runner.hpp"
#include "ipc/socket.hpp"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/vm_sockets.h>
#include <sys/socket.h>

namespace {
    enum class ConnectionMode { Inherited, UnixListener, VsockListener };

    struct StartupOptions {
        std::filesystem::path workspace;
        ConnectionMode mode;
        uint32_t endpoint;
        std::filesystem::path settings;
    };

    auto parse_options(int argc, char **argv) -> StartupOptions {
        ipc::require((argc == 6 || argc == 8) && std::strcmp(argv[1], "--serve") == 0 &&
                         std::strcmp(argv[2], "--workspace") == 0,
                     "usage: agentd --serve --workspace PATH --fd FD | --listen-fd FD | --vsock-port PORT");
        StartupOptions options;
        options.workspace = argv[3];
        const std::string mode = argv[4], value = argv[5];
        ipc::require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
                     "invalid descriptor or port");
        const auto number = std::stoull(value);
        if (mode == "--fd") {
            ipc::require(number >= 3 && number <= INT32_MAX && argc == 6,
                         "invalid inherited socket descriptor or configuration");
            options.mode = ConnectionMode::Inherited;
        } else if (mode == "--listen-fd") {
            ipc::require(number == 3 && argc == 8 &&
                             std::strcmp(argv[6], "--container-config") == 0,
                         "invalid container control listener");
            options.mode = ConnectionMode::UnixListener;
        } else {
            ipc::require(mode == "--vsock-port" && number >= 1024 && number < UINT32_MAX &&
                             (argc == 6 || std::strcmp(argv[6], "--vm-config") == 0),
                         "invalid vsock port or configuration");
            options.mode = ConnectionMode::VsockListener;
        }
        options.endpoint = static_cast<uint32_t>(number);
        if (argc == 8) {
            options.settings = argv[7];
            ipc::require(!options.settings.empty(), "bootstrap configuration path is required");
        }
        return options;
    }

    void serve_listener(lib::UniqueFd listener, const agentd::ServiceConfig &config,
                        ConnectionMode mode) {
        for (;;) {
            sockaddr_vm address{};
            socklen_t size = sizeof(address);
            auto peer = ipc::accept_socket(listener.get(), reinterpret_cast<sockaddr *>(&address),
                                           &size);
            if (mode == ConnectionMode::VsockListener &&
                (size < sizeof(address) || address.svm_family != AF_VSOCK ||
                 address.svm_cid != VMADDR_CID_HOST)) {
                continue;
            }
            try {
                agentd::serve(std::move(peer), config);
            } catch (const std::exception &error) {
                std::cerr << "agentd connection: " << error.what() << '\n';
            }
        }
    }
} // namespace

auto main(int argc, char **argv) -> int {
    // Task mode is a fresh posix_spawn child; it never changes the service's identity.
    if (argc > 1 && std::strcmp(argv[1], "--run-task") == 0) {
        return agentd::run_task(argc - 2, argv + 2);
    }
    try {
        const auto options = parse_options(argc, argv);
        agentd::ServiceConfig config;
        config.workspace = options.workspace;
        if (!options.settings.empty()) {
            if (options.mode == ConnectionMode::UnixListener) {
                agentd::configure_container(config, options.settings);
            } else {
                agentd::configure_microvm(config, options.settings);
            }
        }
        if (options.mode == ConnectionMode::Inherited) {
            agentd::serve(lib::UniqueFd(static_cast<int>(options.endpoint)), config);
        } else if (options.mode == ConnectionMode::UnixListener) {
            lib::UniqueFd listener(static_cast<int>(options.endpoint));
            int accepting = 0;
            socklen_t size = sizeof(accepting);
            ipc::require(getsockopt(listener.get(), SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) == 0 &&
                             accepting,
                         "container control FD is not a listener");
            // Task spawning closes FDs >= 3; its mounts do not expose the socket pathname.
            ipc::require(fcntl(listener.get(), F_SETFD, FD_CLOEXEC) == 0,
                         "protect listener failed");
            serve_listener(std::move(listener), config, options.mode);
        } else {
            serve_listener(ipc::listen_vsock(options.endpoint), config, options.mode);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "agentd: " << error.what() << '\n';
        return 1;
    }
}
