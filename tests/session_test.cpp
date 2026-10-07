#include "agentd/service.hpp"
#include "lib/io.hpp"
#include "lib/socket.hpp"
#include "test_support.hpp"
#include "virtualization/session.hpp"

#include <future>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>

namespace fs = std::filesystem;

// Exercise Session against the real request service, without container/VM isolation.
auto main() -> int {
    try {
        test::TemporaryDirectory temporary;
        const auto workspace = temporary.path / "workspace";
        fs::create_directory(workspace);
        const auto socket = temporary.path / "agentd.sock";
        auto listener = lib::listen_unix(socket);
        agentd::AgentdConfig config;
        config.workspace = workspace;
        config.runtime_info = {{"isolation", "test-service"}};
        config.idle_timeout = std::chrono::seconds(5);
        auto serving = std::async(std::launch::async, [&] {
            lib::wait_fd(listener.get(), POLLIN, lib::Clock::now() + std::chrono::seconds(5));
            const int peer = accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
            test::check(peer >= 0, "accept failed");
            try {
                agentd::serve_agentd(ipc::adopt_descriptor(peer, ipc::DescriptorKind::Socket),
                                     config);
            } catch (const ipc::TransportError &) {
                // Client disconnect ends the service's receive loop.
            }
        });

        virtualization::Session session;
        test::check(!session.is_connected() && !session.cancel(), "new session already connected");
        test::rejects([&] {
            session.ping();
        });
        auto first = session.connect(socket, {}, "test-service", std::chrono::seconds(5));
        auto reused = session.connect(socket, {}, "test-service", std::chrono::seconds(5));
        test::check(first == reused, "connect replaced a live connection");
        first.reset();
        reused.reset();
        test::rejects([&] {
            session.connect(socket, {}, "wrong-isolation", std::chrono::seconds(1));
        });

        const std::string binary("a\0\xff", 3);
        test::check(session.write(workspace / "file", binary).runtime_status == 0, "write failed");
        test::check(session.read(workspace / "file", 1024).out == binary, "binary read failed");
        RuntimeCommand command;
        command.argv = {"/bin/sh", "-c", "printf output; printf diagnostic >&2"};
        command.cwd = workspace;
        std::string streamed_out, streamed_err;
        command.on_output = [&](OutputStream stream, std::string_view bytes) {
            (stream == OutputStream::Stdout ? streamed_out : streamed_err).append(bytes);
        };
        const auto result = session.execute(command);
        test::check(result.runtime_status == 0 && result.out == "output" &&
                        result.err == "diagnostic",
                    "execution result lost");
        test::check(streamed_out == result.out && streamed_err == result.err,
                    "stream callback lost");
        test::check(session.read(workspace / "file", 1024).out == binary,
                    "connection not usable after execution");
        session.ping();
        session.disconnect();
        test::check(!session.is_connected() && !session.cancel(), "disconnect retained the client");
        serving.get();
        std::cout << "session reuse passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
