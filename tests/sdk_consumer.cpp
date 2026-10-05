// This is built as a separate application against the installed SDK.
#include <agent_client.hpp>
#include <sandbox.hpp>
#include <virtualization/microvm/libkrun_runtime.hpp>

#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        return 2;
    }
    const bool vm = argc == 4;
    Sandbox sandbox(argv[1], vm ? make_libkrun_backend() : make_libcrun_backend());
    std::string id;
    try {
        // Verify that the installed low-level Agent client links independently too.
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
            throw std::runtime_error("installed Agent socketpair failed");
        }
        protocol::Client agent(
            protocol::adopt_descriptor(sockets[0], protocol::DescriptorKind::Socket));
        close(sockets[1]);
        bool refused = false;
        try {
            agent.ping();
        } catch (const protocol::ProtocolError &) {
            refused = true;
        }
        if (!refused) {
            throw std::runtime_error("Agent client bypassed handshake");
        }
        Options options;
        options.src_repo = argv[2];
        options.cpu_quota_us = 0; // Explicit installation test policy.
        if (vm) {
            options.environment = Environment::Minimal;
            options.memory_bytes = 512 * 1024 * 1024;
        }
        const auto info = sandbox.create(options);
        id = info.id;
        if (sandbox.read("/workspace/a") != "original\n") {
            throw std::runtime_error("installed SDK read failed");
        }
        const std::string binary("new\0\xff", 5);
        sandbox.write("/workspace/a", binary);
        if (sandbox.read("/workspace/a") != binary) {
            throw std::runtime_error("installed SDK write failed");
        }
        sandbox.stop();
        std::cout << "installed SDK passed\n";
        return 0;
    } catch (const std::exception &error) {
        if (!id.empty()) {
            try {
                sandbox.stop();
            } catch (const std::exception &cleanup) {
                std::cerr << cleanup.what() << '\n';
            }
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
