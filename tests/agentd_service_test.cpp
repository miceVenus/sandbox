#include "test_support.hpp"
#include "virtualization/agentd_client.hpp"

#include <fcntl.h>
#include <future>
#include <iostream>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

namespace fs = std::filesystem;
using test::check;
using test::rejects;

namespace {
    struct AgentdProcess {
        pid_t pid = -1;
        std::unique_ptr<ipc::Transport> transport;

        AgentdProcess(const fs::path &executable, const fs::path &workspace) {
            int sockets[2];
            check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                  "socketpair failed");
            transport = ipc::adopt_descriptor(sockets[0], ipc::DescriptorKind::Socket);
            auto child_owner = ipc::adopt_descriptor(sockets[1], ipc::DescriptorKind::Socket);
            const std::string binary = executable.string(), directory = workspace.string();
            std::vector<char *> argv{const_cast<char *>(binary.c_str()),
                                     const_cast<char *>("--serve"),
                                     const_cast<char *>("--workspace"),
                                     const_cast<char *>(directory.c_str()),
                                     const_cast<char *>("--fd"),
                                     const_cast<char *>("3"),
                                     nullptr};
            char path[] = "PATH=/usr/bin:/bin", locale[] = "LANG=C";
            char *environment[]{path, locale, nullptr};
            posix_spawn_file_actions_t actions;
            check(posix_spawn_file_actions_init(&actions) == 0, "spawn actions failed");
            auto action = [&](int status) {
                if (status != 0) {
                    posix_spawn_file_actions_destroy(&actions);
                    throw std::runtime_error("spawn action failed");
                }
            };
            action(posix_spawn_file_actions_adddup2(&actions, sockets[1], 3));
            action(posix_spawn_file_actions_addclosefrom_np(&actions, 4));
            action(posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0));
            action(posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0));
            const int status =
                posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environment);
            posix_spawn_file_actions_destroy(&actions);
            check(status == 0, "could not start agentd executable");
        }
        ~AgentdProcess() {
            transport.reset();
            if (pid > 0) {
                const auto until = ipc::Clock::now() + std::chrono::seconds(3);
                while (waitpid(pid, nullptr, WNOHANG) == 0) {
                    if (ipc::Clock::now() >= until) {
                        kill(pid, SIGKILL);
                        waitpid(pid, nullptr, 0);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
    };

    ipc::Message hello() {
        const ipc::Limits limits;
        return {0,
                ipc::Flag::Request,
                "core.hello",
                {{"protocol", ipc::protocol_name},
                 {"version", ipc::protocol_version},
                 {"file_bytes", limits.file_bytes},
                 {"stdin_bytes", limits.stdin_bytes},
                 {"output_bytes", limits.output_bytes},
                 {"timeout_ms", limits.timeout_ms}}};
    }
} // namespace

auto main(int argc, char **argv) -> int {
    try {
        check(argc == 2, "agentd executable argument required");
        test::TemporaryDirectory temporary;
        const auto workspace = temporary.path / "B";
        fs::create_directory(workspace);
        fs::create_directory(workspace / "sub");
        test::write(temporary.path / "outside", "host secret");
        test::write(workspace / "original", "original");
        fs::create_symlink(temporary.path / "outside", workspace / "escape");
        fs::create_hard_link(workspace / "original", workspace / "hardlink");
        mkfifo((workspace / "fifo").c_str(), 0600);
        {
            AgentdProcess daemon(argv[1], workspace);
            virtualization::AgentdClient client(std::move(daemon.transport));
            rejects([&] {
                client.ping();
            });
            client.handshake();
            client.ping();
            std::string content(200000, '\0');
            for (size_t i = 0; i < content.size(); ++i) {
                content[i] = static_cast<char>(i);
            }
            client.write(workspace / "sub/binary", content);
            check(client.read(workspace / "sub/binary", content.size()) == content,
                  "chunked binary round trip failed");
            client.write(workspace / "empty", {});
            check(client.read(workspace / "empty", 1).empty(), "empty file failed");
            for (const auto &path : {workspace / "escape", workspace / "../outside",
                                     temporary.path / "outside", workspace / "fifo"}) {
                rejects([&] {
                    client.read(path, 1024);
                });
                rejects([&] {
                    client.write(path, "changed");
                });
                client.ping(); // Operation errors do not corrupt a healthy connection.
            }
            rejects([&] {
                client.write(workspace / "hardlink", "changed");
            });
            check(test::read(temporary.path / "outside") == "host secret",
                  "file API touched host path");

            RuntimeCommand command{{"/bin/sh", "-c", "printf out; printf err >&2; pwd; exit 7"},
                                   workspace / "sub"};
            std::string streamed_out, streamed_err;
            auto result = client.execute(command, [&](bool err, std::string_view data) {
                (err ? streamed_err : streamed_out).append(data);
            });
            check(result.runtime_status == 7 &&
                      result.out == "out" + (workspace / "sub").string() + "\n" &&
                      result.err == "err",
                  "exit status, streams or cwd lost");
            check(streamed_out == result.out && streamed_err == result.err,
                  "streaming callback lost output");
            command = {{"/bin/cat"}, workspace};
            command.stdin_data = content;
            result = client.execute(command);
            check(result.runtime_status == 0 && result.out == content, "binary stdin failed");
            command = {{"/bin/sh", "-c", "sleep 5"}, workspace};
            command.timeout_ms = 60;
            result = client.execute(command);
            check(result.timed_out && result.runtime_status == 137,
                  "agentd did not terminate timed out command");
            client.ping();
            command = {{"/bin/sh", "-c", "while :; do printf 0123456789; done"}, workspace};
            command.output_limit = 1234;
            result = client.execute(command);
            check(result.output_limited && result.out.size() + result.err.size() <= 1234,
                  "output cap lost");
            command = {{"/bin/sh", "-c", "sleep 5"}, workspace};
            command.timeout_ms = 10000;
            auto execution = std::async(std::launch::async, [&] {
                return client.execute(command);
            });
            const auto until = ipc::Clock::now() + std::chrono::seconds(1);
            bool cancelled = false;
            while (ipc::Clock::now() < until && !(cancelled = client.cancel())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(cancelled && execution.get().cancelled, "concurrent cancellation failed");
            client.ping();
            command = {{"/definitely/missing"}, workspace};
            rejects([&] {
                client.execute(command);
            });
            client.ping();
            command = {{"/bin/sh", "-c", "test ! -e /proc/self/fd/3"}, workspace};
            check(client.execute(command).runtime_status == 0, "task inherited control descriptor");
        }
        // Disconnect before write.end: original file survives and staging file is removed.
        {
            AgentdProcess daemon(argv[1], workspace);
            {
                ipc::Channel channel(std::move(daemon.transport));
                const auto until = ipc::Clock::now() + std::chrono::seconds(2);
                channel.send(hello(), until);
                check(channel.receive(until).type == "core.ready", "raw handshake failed");
                channel.send({1,
                              ipc::Flag::Request,
                              "fs.write",
                              {{"path", (workspace / "original").string()}, {"size", size_t(6)}}},
                             until);
                check(channel.receive(until).type == "core.error",
                      "hard-linked write was accepted");
                channel.send({2,
                              ipc::Flag::Request,
                              "fs.write",
                              {{"path", (workspace / "empty").string()}, {"size", size_t(6)}}},
                             until);
                check(channel.receive(until).type == "fs.write.accepted", "write was not accepted");
                channel.send({2,
                              ipc::Flag::Event,
                              "fs.write.data",
                              {{"offset", size_t(0)}, {"data", ipc::binary_bytes("partial")}}},
                             until);
                // Exceeds declared size: connection closes and no data becomes visible.
                rejects([&] {
                    channel.receive(until);
                });
            }
        }
        check(test::read(workspace / "empty").empty(), "abandoned upload changed target");
        test::write(workspace / "retained", "kept");
        {
            AgentdProcess daemon(argv[1], workspace);
            {
                ipc::Channel channel(std::move(daemon.transport));
                const auto until = ipc::Clock::now() + std::chrono::seconds(2);
                channel.send(hello(), until);
                channel.receive(until);
                channel.send({1,
                              ipc::Flag::Request,
                              "fs.write",
                              {{"path", (workspace / "retained").string()}, {"size", size_t(6)}}},
                             until);
                check(channel.receive(until).type == "fs.write.accepted", "write was not accepted");
                channel.send({1,
                              ipc::Flag::Event,
                              "fs.write.data",
                              {{"offset", size_t(0)}, {"data", ipc::binary_bytes("part")}}},
                             until);
                // Close without write.end, even though these chunks are well formed.
            }
        }
        check(test::read(workspace / "retained") == "kept",
              "disconnect replaced the original file");
        {
            AgentdProcess daemon(argv[1], workspace);
            {
                ipc::Channel channel(std::move(daemon.transport));
                const auto until = ipc::Clock::now() + std::chrono::seconds(2);
                channel.send(hello(), until);
                channel.receive(until);
                channel.send({1,
                              ipc::Flag::Request,
                              "exec.start",
                              {{"argv", {"/bin/sh", "-c", "echo $$ > task.pid; sleep 10"}},
                               {"cwd", workspace.string()},
                               {"stdin_bytes", size_t(0)},
                               {"timeout_ms", uint32_t(20000)},
                               {"output_bytes", size_t(1024)}}},
                             until);
                check(channel.receive(until).type == "exec.accepted", "exec not accepted");
                channel.send({1, ipc::Flag::Event, "exec.stdin.end", ipc::Json::object()}, until);
                check(channel.receive(until).type == "exec.started", "exec not started");
                while (!fs::exists(workspace / "task.pid") && ipc::Clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                check(fs::exists(workspace / "task.pid"), "actual task did not start");
            }
        }
        const auto task_pid = std::stoi(test::read(workspace / "task.pid"));
        check(kill(task_pid, 0) < 0 && errno == ESRCH, "agentd did not reap task after disconnect");
        for (const auto &entry : fs::directory_iterator(workspace)) {
            check(entry.path().filename().string().rfind(".sandbox-io-", 0) != 0,
                  "staging file leaked");
        }
        {
            AgentdProcess daemon(argv[1], workspace);
            {
                ipc::Channel channel(std::move(daemon.transport));
                const auto until = ipc::Clock::now() + std::chrono::seconds(2);
                channel.send(hello(), until);
                channel.receive(until);
                channel.send({1, ipc::Flag::Request, "unknown.operation", ipc::Json::object()},
                             until);
                check(channel.receive(until).type == "core.error",
                      "unknown operation was accepted");
                channel.send({1, ipc::Flag::Request, "core.ping", ipc::Json::object()}, until);
                rejects([&] {
                    channel.receive(until);
                }); // IDs must never be reused.
            }
        }
        std::cout << "agentd service passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
