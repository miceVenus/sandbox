#include "agentd/service.hpp"
#include "ipc/io.hpp"
#include "ipc/socket.hpp"
#include "test_support.hpp"
#include "virtualization/agentd_client.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>

namespace fs = std::filesystem;

// Exercise the client-owned Session against the real request service.
auto main() -> int {
    try {
        test::TemporaryDirectory temporary;
        const auto workspace = temporary.path / "workspace";
        fs::create_directory(workspace);
        const auto socket = temporary.path / "agentd.sock";
        auto listener = ipc::listen_unix(socket);
        agentd::ServiceConfig config;
        config.workspace = workspace;
        config.runtime_info = {{"isolation", "test-service"}};
        config.idle_timeout = std::chrono::seconds(5);
        std::atomic<int> cleanup_calls{0};
        config.cleanup_tasks = [&] { ++cleanup_calls; };
        auto serving = std::async(std::launch::async, [&] {
            for (int connection = 0; connection < 3; ++connection) {
                ipc::wait_fd(listener.get(), POLLIN, ipc::Clock::now() + std::chrono::seconds(5));
                const int peer = accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
                test::check(peer >= 0, "accept failed");
                try {
                    agentd::serve(lib::UniqueFd(peer), config);
                } catch (const ipc::IoError &) {
                    // Client disconnect ends the service's receive loop.
                }
            }
        });

        virtualization::AgentdClient client;
        test::check(!client.is_connected() && !client.cancel(), "new session already connected");
        test::rejects([&] {
            client.ping();
        });
        client.connect(socket, {}, "test-service", std::chrono::seconds(5));
        client.connect(socket, {}, "test-service", std::chrono::seconds(5));
        client.ping(); // Reuse must not send a second handshake.
        test::rejects([&] {
            client.connect(socket, {}, "wrong-isolation", std::chrono::seconds(1));
        });

        const std::string binary("a\0\xff", 3);
        test::check(client.write_result(workspace / "file", binary).runtime_status == 0, "write failed");
        test::check(client.read_result(workspace / "file", 1024).out == binary, "binary read failed");
        RuntimeCommand command;
        command.argv = {"/bin/sh", "-c", "printf output; printf diagnostic >&2"};
        command.cwd = workspace;
        std::string streamed_out, streamed_err;
        command.on_output = [&](OutputStream stream, std::string_view bytes) {
            (stream == OutputStream::Stdout ? streamed_out : streamed_err).append(bytes);
        };
        const auto result = client.execute_result(command);
        test::check(result.runtime_status == 0 && result.out == "output" &&
                        result.err == "diagnostic",
                    "execution result lost");
        test::check(streamed_out == result.out && streamed_err == result.err,
                    "stream callback lost");
        test::check(client.read_result(workspace / "file", 1024).out == binary,
                    "connection not usable after execution");
        const auto missing = client.read_result(workspace / "missing", 1024);
        test::check(missing.runtime_status == 1 && !missing.err.empty(),
                    "remote error Result translation lost");
        client.ping();
        command.argv = {"/missing-executable"};
        test::check(client.execute_result(command).runtime_status == 1,
                    "failed spawn was not reported");
        test::check(cleanup_calls == 2, "execution and failed spawn must each clean up once");
        client.ping();
        client.disconnect();
        test::check(!client.is_connected() && !client.cancel(), "disconnect retained the client");
        test::rejects([&] {
            client.connect(socket, {}, "wrong-isolation", std::chrono::seconds(5));
        });
        test::check(!client.is_connected(), "failed handshake retained a connection");
        client.connect(socket, {}, "test-service", std::chrono::seconds(5));
        client.ping(); // Reconnection starts request IDs from the beginning.
        test::check(client.read_result(workspace / "file", 1024).out == binary,
                    "reconnected client lost file access");
        client.disconnect();
        serving.get();

        // A disconnect after cleanup but before the terminal reply must not run cleanup twice.
        int abandoned[2];
        test::check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, abandoned) == 0,
                    "socketpair failed");
        std::promise<void> cleanup_entered, release_cleanup;
        auto entered = cleanup_entered.get_future();
        auto released = release_cleanup.get_future().share();
        std::atomic<int> abandoned_cleanup_calls{0};
        auto abandoned_config = config;
        abandoned_config.cleanup_tasks = [&] {
            if (++abandoned_cleanup_calls == 1) {
                cleanup_entered.set_value();
            }
            test::check(released.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                        "cleanup gate timed out");
        };
        auto abandoned_service = std::async(std::launch::async, [&] {
            try {
                agentd::serve(lib::UniqueFd(abandoned[1]), abandoned_config);
            } catch (const ipc::IoError &) {
            }
        });
        ipc::Session abandoned_client{lib::UniqueFd(abandoned[0])};
        const auto abandoned_until = ipc::Clock::now() + std::chrono::seconds(3);
        abandoned_client.send({0, ipc::Flag::Request, "core.hello",
                               {{"protocol", ipc::protocol_name}, {"version", ipc::protocol_version},
                                {"file_bytes", 1024}, {"stdin_bytes", 1024},
                                {"output_bytes", 1024}, {"timeout_ms", 1000}}}, abandoned_until);
        test::check(abandoned_client.receive(abandoned_until).type == "core.ready",
                    "cleanup fixture handshake failed");
        abandoned_client.send({1, ipc::Flag::Request, "exec.start",
                               {{"argv", ipc::Json::array({"/bin/true"})},
                                {"cwd", workspace.string()}, {"stdin_bytes", 0},
                                {"output_bytes", 1024}, {"timeout_ms", 1000}}}, abandoned_until);
        test::check(abandoned_client.receive(abandoned_until).type == "exec.accepted",
                    "cleanup fixture rejected command");
        abandoned_client.send({1, ipc::Flag::Event, "exec.stdin.end", ipc::Json::object()},
                              abandoned_until);
        test::check(abandoned_client.receive(abandoned_until).type == "exec.started",
                    "cleanup fixture did not start command");
        test::check(entered.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                    "cleanup was not reached");
        abandoned_client.disconnect();
        release_cleanup.set_value();
        abandoned_service.get();
        test::check(abandoned_cleanup_calls == 1, "terminal reply failure repeated cleanup");

        int pair[2];
        test::check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0,
                    "socketpair failed");
        ipc::Session sender{lib::UniqueFd(pair[0])};
        ipc::Session receiver{lib::UniqueFd(pair[1])};
        const auto until = ipc::Clock::now() + std::chrono::seconds(1);
        sender.send({7, ipc::Flag::Event, "test.message", {{"value", "communication only"}}}, until);
        const auto message = receiver.receive(until);
        test::check(message.id == 7 && message.payload.at("value") == "communication only",
                    "Session required agentd handshake or changed message semantics");
        // Concurrent senders must preserve complete frames, even under socket backpressure.
        const auto concurrent_until = ipc::Clock::now() + std::chrono::seconds(3);
        const std::string payload(4096, 'x');
        auto send_batch = [&](uint32_t first_id) {
            for (uint32_t i = 0; i < 64; ++i) {
                sender.send({first_id + i, ipc::Flag::Event, "test.concurrent",
                             {{"data", ipc::binary_bytes(payload)}}}, concurrent_until);
            }
        };
        auto first_writer = std::async(std::launch::async, send_batch, 1);
        auto second_writer = std::async(std::launch::async, send_batch, 65);
        std::vector<bool> received(129, false);
        for (int i = 0; i < 128; ++i) {
            const auto item = receiver.receive(concurrent_until);
            test::check(item.id >= 1 && item.id <= 128 && !received[item.id] &&
                            ipc::binary_string(item.payload.at("data"), payload.size()) == payload,
                        "concurrent sends interleaved or lost frames");
            received[item.id] = true;
        }
        first_writer.get();
        second_writer.get();
        sender.invalidate();
        test::rejects([&] {
            sender.send({8, ipc::Flag::Event, "test.message", ipc::Json::object()}, until);
        });
        sender.disconnect();
        test::check(!sender.is_connected(), "Session disconnect retained stream");
        std::cout << "session reuse passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
