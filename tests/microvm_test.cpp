#include "ipc/session.hpp"
#include "ipc/socket.hpp"
#include "sandbox.hpp"
#include "test_support.hpp"
#include "virtualization/microvm/krun_runtime.hpp"
#include <iostream>

namespace fs = std::filesystem;
using test::check;
using test::rejects;

auto main(int argc, char **argv) -> int {
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            if (!path.empty()) {
                std::error_code ec;
                fs::remove_all(path, ec);
            }
        }
    } cleanup;
    std::string id;
    std::string phase = "create VM";
    std::unique_ptr<Sandbox> manager;
    try {
        char pattern[] = "/tmp/krun-sdk-XXXXXX";
        const auto created = mkdtemp(pattern);
        check(created != nullptr, "temporary directory failed");
        const fs::path root = created;
        cleanup.path = root;
        const auto source = root / "A";
        fs::create_directory(source);
        test::git(source, {"init", "-q"});
        test::write(source / "a", "original\n");
        test::commit(source, "baseline");
        manager = std::make_unique<Sandbox>(root / "state", make_krun_backend());
        Options options;
        options.src_repo = source;
        options.ctr_repo = "/project";
        options.environment = argc > 1 && std::string(argv[1]) == "host-tools"
                                  ? Environment::HostTools
                                  : Environment::Minimal;
        options.memory_bytes = 512 * 1024 * 1024;
        options.cpu_quota_us = 0; // This machine delegates memory/pids, not cpu.
        options.cmd_timeout = std::chrono::seconds(10);
        const auto info = manager->create(options);
        id = info.id;
        phase = "streaming and cancellation";
        check(info.runtime_backend == "vm-libkrun" && info.rootless &&
                  info.resource_limits_verified,
              "VM policy not verified");
        std::string streamed_out, streamed_err;
        const auto streamed = manager->execute(
            std::vector<std::string>{"/bin/sh", "-c",
                                     "printf first; sleep 0.1; printf second; printf error >&2"},
            [&](OutputStream stream, std::string_view bytes) {
                (stream == OutputStream::Stdout ? streamed_out : streamed_err).append(bytes);
            });
        check(streamed.runtime_status == 0 && streamed_out == "firstsecond" &&
                  streamed_err == "error" && streamed.out == streamed_out &&
                  streamed.err == streamed_err,
              "SDK streaming lost output");
        bool cancellation_sent = false;
        const auto cancelled = manager->execute(
            std::vector<std::string>{"/bin/sh", "-c", "printf ready; sleep 9"},
            [&](OutputStream stream, std::string_view bytes) {
                if (stream == OutputStream::Stdout && !bytes.empty() && !cancellation_sent)
                    cancellation_sent = manager->cancel();
            });
        check(cancellation_sent && cancelled.cancelled && !cancelled.timed_out,
              "streaming cancellation did not reclaim the task");
        auto exec = [&](std::vector<std::string> args) {
            const auto result = manager->execute(args);
            check(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
                  "Guest command failed: " + result.err);
            return result.out;
        };
        phase = "Guest task policy";
        const auto status = exec({"/bin/cat", "/proc/self/status"});
        check(status.find("Uid:\t65534\t65534") != std::string::npos &&
                  status.find("CapEff:\t0000000000000000") != std::string::npos &&
                  status.find("NoNewPrivs:\t1") != std::string::npos,
              "Guest task privileges wrong");
        check(exec({"/bin/pwd"}) == "/project\n", "Guest cwd wrong");
        const auto expected_environment = options.environment == Environment::HostTools
            ? "/env/home|/env/python/bin:/usr/bin:/bin|/build/tmp|/cache"
            : "/project|/bin:/usr/bin||";
        check(exec({"/bin/sh", "-c",
                    "printf '%s|%s|%s|%s' \"$HOME\" \"$PATH\" \"$TMPDIR\" \"$XDG_CACHE_HOME\""}) ==
                  expected_environment,
              "Guest task environment does not match the selected environment");
        check(exec({"/bin/cat", "/sys/fs/cgroup/sandbox-tasks/pids.max"}) == "64\n",
              "Guest pids limit missing");
        exec({"/bin/sh", "-c",
              "test ! -e '" + source.string() +
                  "/a'; "
                  "test ! -e /dev/kvm; test ! -e /control/agentd.sock; "
                  "test ! -e /project/manager.git"});
        exec({"/bin/sh", "-c",
              "! kill -9 1; ! touch /sandbox-tools/bad; "
              "! echo bad > /sys/fs/cgroup/sandbox-tasks/pids.max"});
        exec({"/bin/sh", "-c",
              "service=; for p in /proc/[0-9]*/comm; do "
              "IFS= read -r name < \"$p\" || continue; "
              "if [ \"$name\" = agentd ]; then service=${p%/comm}; "
              "service=${service##*/}; break; fi; done; "
              "test -n \"$service\" && ! kill -TERM \"$service\""});
        const auto denied =
            manager->execute({"/sandbox-tools/agentd", "--run-task", "--", "/bin/true"});
        check(denied.runtime_status == 126 && !denied.timed_out,
              "unprivileged command re-entered privileged task mode");
        exec({"/bin/sh", "-c",
              "test \"$1\" = '--serve' && test \"$2\" = '--run-task' && test \"$3\" = 'a b'",
              "argument-test", "--serve", "--run-task", "a b"});
        check(!fs::exists(info.directory / "guest/rootfs/sandbox-tools/sandbox-task"),
              "VM still deploys a separate task executable");
        phase = "file API";
        manager->write("/project/a", "changed\n");
        check(manager->read("/project/a") == "changed\n", "VM file API failed");
        const std::string binary("a\0b\xff", 4);
        manager->write("/project/binary", binary);
        check(manager->read("/project/binary") == binary, "VM binary file roundtrip failed");
        phase = "disconnected upload";
        // agentd serves one owned connection at a time. Release the persistent SDK
        // channel before the low-level fixture, without stopping the running VM.
        manager.reset();
        {
            // Disconnect in the middle of an upload. The privileged service must
            // remove its temporary file using the mapped file identity too.

            const auto until = ipc::Clock::now() + std::chrono::seconds(5);
            ipc::Session channel(ipc::connect_unix(info.directory / "control/agentd.sock", until));
            channel.send({0,
                          ipc::Flag::Request,
                          "core.hello",
                          {{"protocol", ipc::protocol_name},
                           {"version", ipc::protocol_version},
                           {"file_bytes", 1024},
                           {"stdin_bytes", 1024},
                           {"output_bytes", 1024},
                           {"timeout_ms", 2000}}},
                         until);
            check(channel.receive(until).type == "core.ready", "upload fixture handshake failed");
            channel.send({1,
                          ipc::Flag::Request,
                          "fs.write",
                          {{"path", "/project/abandoned"}, {"size", 100}}},
                         until);
            check(channel.receive(until).type == "fs.write.accepted", "upload fixture rejected");
            channel.send({1,
                          ipc::Flag::Event,
                          "fs.write.data",
                          {{"offset", 0}, {"data", ipc::binary_bytes("partial")}}},
                         until);
        }
        manager = std::make_unique<Sandbox>(root / "state", make_krun_backend());
        manager->open(id);
        phase = "upload cleanup and file boundaries";
        check(exec({"/bin/sh", "-c", "find /project -name '.sandbox-io-*'"}).empty(),
              "disconnected VM upload left a temporary file");
        exec({"/bin/test", "!", "-e", "/project/abandoned"});
        rejects([&] {
            manager->read("/etc/passwd");
        });
        rejects([&] {
            manager->write("/project/../bad", "bad");
        });
        exec({"/bin/ln", "-s", "/tmp", "/project/escape"});
        rejects([&] {
            manager->write("/project/escape/bad", "bad");
        });
        exec({"/bin/rm", "/project/escape"});
        CommandRequest input{{"/bin/cat"}, std::nullopt, binary};
        check(manager->execute(input).out == binary, "VM stdin failed");
        // A detached child must remain in the Guest task cgroup and be reclaimed.
        exec({"/bin/sh", "-c",
              "setsid /bin/sh -c 'sleep 2; echo escaped > /project/escaped' "
              "</dev/null >/dev/null 2>&1 &"});
        exec({"/bin/sleep", "3"});
        exec({"/bin/test", "!", "-e", "/project/escaped"});
        if (options.environment == Environment::HostTools) {
            manager->write("/project/main.cpp",
                           "#include <iostream>\nint main(){std::cout << 42;}\n");
            exec({"/usr/bin/g++", "/project/main.cpp", "-o", "/build/app"});
            check(exec({"/build/app"}) == "42", "Guest compiler environment failed");
        }
        phase = "workspace inspection";
        const auto changes = manager->get_changes();
        check(changes.diff.find("+changed") != std::string::npos, "Guest sync/diff failed");
        check(manager->get_status() == SandboxState::Active, "VM did not resume");
        check(test::read(source / "a") == "original\n", "A changed");
        // Re-open without retaining an in-memory VMM PID or client connection.
        phase = "reopen VM";
        manager.reset();
        manager = std::make_unique<Sandbox>(root / "state", make_krun_backend());
        manager->open(info.id);
        check(manager->read("/project/a") == "changed\n", "VM re-open failed");
        manager->stop();
        manager->stop();
        check(manager->get_status() == SandboxState::Stopped, "VM stop failed");
        check(manager->get_changes().diff.find("+changed") != std::string::npos,
              "stopped VM lost B");
        manager->destroy();
        check(!fs::exists(info.directory), "destroy retained B or info artifacts");
        id.clear();
        if (options.environment == Environment::Minimal) {
            phase = "VM timeout policy";
            options.cmd_timeout = std::chrono::milliseconds(200);
            manager = std::make_unique<Sandbox>(root / "state", make_krun_backend());
            const auto timed = manager->create(options);
            id = timed.id;
            const auto result = manager->execute({"/bin/sleep", "10"});
            check(result.timed_out && manager->get_status() == SandboxState::Failed,
                  "VM command timeout did not fail the info");
            check(!fs::exists(root / "state/runtime" / timed.runtime_id),
                  "timeout left VMM running");
            manager->destroy();
            id.clear();

            options.cmd_timeout = std::chrono::seconds(10);
            phase = "VM output policy";
            options.max_output_bytes = 64;
            manager = std::make_unique<Sandbox>(root / "state", make_krun_backend());
            const auto bounded = manager->create(options);
            id = bounded.id;
            const auto noisy = manager->execute({"/bin/sh", "-c", "yes x"});
            check(noisy.output_limited && noisy.out.size() + noisy.err.size() <= 64 &&
                      manager->get_status() == SandboxState::Failed,
                  "VM output limit failed");
            check(!fs::exists(root / "state/runtime" / bounded.runtime_id),
                  "output limit left VMM running");
            manager->destroy();
            id.clear();
        }
        std::cout << "real libkrun VM SDK test passed\n";
    } catch (const std::exception &error) {
        std::cerr << phase << ": " << error.what() << '\n';
        if (manager && !id.empty()) {
            try {
                manager->stop();
            } catch (const std::exception &cleanup) {
                std::cerr << "cleanup: " << cleanup.what() << '\n';
            }
        } else if (!id.empty()) {
            try {
                Sandbox recovery(cleanup.path / "state", make_krun_backend());
                recovery.open(id);
                recovery.stop();
            } catch (const std::exception &cleanup_error) {
                std::cerr << "cleanup: " << cleanup_error.what() << '\n';
            }
        }
        return 1;
    }
}
