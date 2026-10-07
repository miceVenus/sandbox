// SDK-internal service, deployed as container PID 1 or inside a Guest.
#include "agentd/container_bootstrap.hpp"
#include "agentd/microvm_bootstrap.hpp"
#include "agentd/service.hpp"
#include "agentd/task_runner.hpp"
#include "lib/socket.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <unistd.h>

auto main(int argc, char **argv) -> int {
    // Dispatch before creating a transport or a worker thread. Task mode is a
    // fresh posix_spawn child, never a privilege change in the serving process.
    if (argc > 1 && std::strcmp(argv[1], "--run-task") == 0) {
        return agentd::run_task(argc - 2, argv + 2);
    }
    try {
        ipc::require(argc > 1 && std::strcmp(argv[1], "--serve") == 0,
                     "agentd requires --serve; commands arrive over its private transport");
        --argc;
        ++argv;
        ipc::require(
            argc == 5 || argc == 7,
            "usage: agentd --serve --workspace PATH --vsock-port PORT | --serial DEVICE | --fd FD");
        ipc::require(std::string(argv[1]) == "--workspace",
                     "workspace must be configured by the launcher");
        agentd::AgentdConfig config;
        config.workspace = argv[2];
        if (argc == 7) {
            if (std::string(argv[5]) == "--vm-config") {
                agentd::configure_microvm(config, argv[6]);
            } else {
                ipc::require(std::string(argv[5]) == "--container-config",
                             "invalid bootstrap option");
                agentd::configure_container(config, argv[6]);
            }
        }
        const std::string mode = argv[3], value = argv[4];
        if (mode == "--serial") {
            auto transport =
                ipc::adopt_descriptor(open(value.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC),
                                      ipc::DescriptorKind::SerialPort);
            agentd::serve_agentd(std::move(transport), config);
            return 0;
        }
        ipc::require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
                     "invalid descriptor or port");
        const auto number = std::stoull(value);
        if (mode == "--listen-fd") {
            ipc::require(argc == 7 && std::string(argv[5]) == "--container-config" && number == 3,
                         "invalid container control listener");
            const int listener = static_cast<int>(number);
            int accepting = 0;
            socklen_t accepting_size = sizeof(accepting);
            ipc::require(
                getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_size) == 0 &&
                    accepting,
                "container control FD is not a listener");
            // Each task spawn closes FDs >= 3. The socket pathname exists only in
            // the host mount namespace, so tasks cannot open a new control connection.
            ipc::require(fcntl(listener, F_SETFD, FD_CLOEXEC) == 0, "protect listener failed");
            for (;;) {
                auto peer = lib::accept_socket(listener);
                try {
                    agentd::serve_agentd(
                        ipc::adopt_descriptor(peer.release(), ipc::DescriptorKind::Socket), config);
                } catch (const std::exception &error) {
                    std::cerr << "agentd connection: " << error.what() << '\n';
                }
            }
        }
        if (mode == "--fd") {
            ipc::require(number >= 3 && number <= INT32_MAX, "invalid inherited socket descriptor");
            agentd::serve_agentd(
                ipc::adopt_descriptor(static_cast<int>(number), ipc::DescriptorKind::Socket),
                config);
            return 0;
        }
        ipc::require(mode == "--vsock-port" && number >= 1024 && number < UINT32_MAX,
                     "invalid vsock port");
        auto listener = lib::listen_vsock(static_cast<uint32_t>(number));
        for (;;) {
            sockaddr_vm peer_address{};
            socklen_t peer_size = sizeof(peer_address);
            auto peer = lib::accept_socket(listener.get(),
                                           reinterpret_cast<sockaddr *>(&peer_address), &peer_size);
            if (peer_size < sizeof(peer_address) || peer_address.svm_family != AF_VSOCK ||
                peer_address.svm_cid != VMADDR_CID_HOST) {
                continue;
            }
            try {
                agentd::serve_agentd(
                    ipc::adopt_descriptor(peer.release(), ipc::DescriptorKind::Socket), config);
            } catch (const std::exception &error) {
                std::cerr << "agentd connection: " << error.what() << '\n';
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "agentd: " << error.what() << '\n';
        return 1;
    }
}
