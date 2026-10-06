// SDK-internal service, deployed as container PID 1 or inside a Guest.
#include "agent/service.hpp"
#include "agent/container_bootstrap.hpp"
#include "virtualization/microvm/guest/guest_vm.hpp"
#include "agent/task_runner.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <unistd.h>

namespace agent = protocol;

auto main(int argc, char **argv) -> int {
    // Dispatch before creating a transport or a worker thread. Task mode is a
    // fresh posix_spawn child, never a privilege change in the serving process.
    if (argc > 1 && std::strcmp(argv[1], "--run-task") == 0) {
        return agent::run_agent_task(argc - 2, argv + 2);
    }
    try {
        agent::require(argc > 1 && std::strcmp(argv[1], "--serve") == 0,
                       "agentd requires --serve; commands arrive over its private transport");
        --argc;
        ++argv;
        agent::require(
            argc == 5 || argc == 7,
            "usage: agentd --serve --workspace PATH --vsock-port PORT | --serial DEVICE | --fd FD");
        agent::require(std::string(argv[1]) == "--workspace",
                       "workspace must be configured by the launcher");
        agent::AgentConfig config;
        config.workspace = argv[2];
        if (argc == 7) {
            if (std::string(argv[5]) == "--vm-config") {
                agent::configure_guest_vm(config, argv[6]);
            } else {
                agent::require(std::string(argv[5]) == "--container-config", "invalid bootstrap option");
                agent::configure_container_agent(config, argv[6]);
            }
        }
        const std::string mode = argv[3], value = argv[4];
        if (mode == "--serial") {
            auto transport =
                agent::adopt_descriptor(open(value.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC),
                                        agent::DescriptorKind::SerialPort);
            agent::serve_agent(std::move(transport), config);
            return 0;
        }
        agent::require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
                       "invalid descriptor or port");
        const auto number = std::stoull(value);
        if (mode == "--listen-fd") {
            agent::require(argc == 7 && std::string(argv[5]) == "--container-config" &&
                               number == 3, "invalid container control listener");
            const int listener = static_cast<int>(number);
            int accepting = 0;
            socklen_t accepting_size = sizeof(accepting);
            agent::require(getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                                     &accepting_size) == 0 && accepting,
                           "container control FD is not a listener");
            // Each task spawn closes FDs >= 3. The socket pathname exists only in
            // the host mount namespace, so tasks cannot open a new control connection.
            agent::require(fcntl(listener, F_SETFD, FD_CLOEXEC) == 0, "protect listener failed");
            for (;;) {
                const int peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                if (peer < 0 && errno == EINTR) continue;
                agent::require(peer >= 0, "accept container control failed");
                try {
                    agent::serve_agent(agent::adopt_descriptor(peer, agent::DescriptorKind::Socket), config);
                } catch (const std::exception &error) {
                    std::cerr << "agentd connection: " << error.what() << '\n';
                }
            }
        }
        if (mode == "--fd") {
            agent::require(number >= 3 && number <= INT32_MAX,
                           "invalid inherited socket descriptor");
            agent::serve_agent(
                agent::adopt_descriptor(static_cast<int>(number), agent::DescriptorKind::Socket),
                config);
            return 0;
        }
        agent::require(mode == "--vsock-port" && number >= 1024 && number < UINT32_MAX,
                       "invalid vsock port");
        const int listener = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0) {
            throw std::runtime_error("create vsock listener: " + std::string(std::strerror(errno)));
        }
        auto owner = agent::adopt_descriptor(listener, agent::DescriptorKind::Socket);
        // Listener accept is blocking; connected transports become nonblocking.
        const auto flags = fcntl(listener, F_GETFL);
        agent::require(flags >= 0 && fcntl(listener, F_SETFL, flags & ~O_NONBLOCK) == 0,
                       "configure listener failed");
        sockaddr_vm address{};
        address.svm_family = AF_VSOCK;
        address.svm_cid = VMADDR_CID_ANY;
        address.svm_port = static_cast<uint32_t>(number);
        agent::require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                               0 &&
                           listen(listener, 4) == 0,
                       "bind/listen vsock failed");
        for (;;) {
            sockaddr_vm peer_address{};
            socklen_t peer_size = sizeof(peer_address);
            const int peer = accept4(
                listener, reinterpret_cast<sockaddr *>(&peer_address), &peer_size, SOCK_CLOEXEC);
            if (peer < 0 && errno == EINTR) {
                continue;
            }
            agent::require(peer >= 0, "accept vsock failed");
            if (peer_size < sizeof(peer_address) || peer_address.svm_family != AF_VSOCK ||
                peer_address.svm_cid != VMADDR_CID_HOST) {
                close(peer);
                continue;
            }
            try {
                agent::serve_agent(agent::adopt_descriptor(peer, agent::DescriptorKind::Socket),
                                   config);
            } catch (const std::exception &error) {
                std::cerr << "agentd connection: " << error.what() << '\n';
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "agentd: " << error.what() << '\n';
        return 1;
    }
}
