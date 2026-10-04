#include "../include/communication/agent_transport.hpp"
#include "../include/sandbox.hpp"
#include "../include/virtualization/microvm/libkrun_runtime.hpp"
#include "test_support.hpp"
#include <iostream>

namespace fs = std::filesystem;
using test::check;
using test::rejects;

int main(int argc, char **argv) {
    struct Cleanup {
        fs::path path;
        ~Cleanup() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
    } cleanup;
    std::string id;
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
        manager = std::make_unique<Sandbox>(root / "state", make_libkrun_backend());
        Options options;
        options.src_repo = source;
        options.ctr_repo = "/project";
        options.environment = argc > 1 && std::string(argv[1]) == "host-tools"
                                  ? Environment::HostTools : Environment::Minimal;
        options.memory_bytes = 512 * 1024 * 1024;
        options.cpu_quota_us = 0; // This machine delegates memory/pids, not cpu.
        options.cmd_timeout = std::chrono::seconds(10);
        const auto info = manager->create(options);
        id = info.id;
        check(info.runtime_backend == "vm-libkrun" && info.rootless &&
              info.resource_limits_verified, "VM policy not verified");
        auto exec = [&](std::vector<std::string> args) {
            const auto result = manager->execute(args);
            check(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
                  "Guest command failed: " + result.err);
            return result.out;
        };
        const auto status = exec({"/bin/cat", "/proc/self/status"});
        check(status.find("Uid:\t65534\t65534") != std::string::npos &&
              status.find("CapEff:\t0000000000000000") != std::string::npos &&
              status.find("NoNewPrivs:\t1") != std::string::npos, "Guest task privileges wrong");
        check(exec({"/bin/pwd"}) == "/project\n", "Guest cwd wrong");
        check(exec({"/bin/cat", "/sys/fs/cgroup/sandbox-tasks/pids.max"}) == "64\n",
              "Guest pids limit missing");
        exec({"/bin/sh", "-c", "test ! -e '" + source.string() + "/a'; "
                               "test ! -e /dev/kvm; test ! -e /control/agent.sock; "
                               "test ! -e /project/manager.git"});
        exec({"/bin/sh", "-c", "! kill -9 1; ! touch /sandbox-tools/bad; "
                               "! echo bad > /sys/fs/cgroup/sandbox-tasks/pids.max"});
        exec({"/bin/sh", "-c",
              "service=; for p in /proc/[0-9]*/comm; do "
              "IFS= read -r name < \"$p\" || continue; "
              "if [ \"$name\" = agentd ]; then service=${p%/comm}; "
              "service=${service##*/}; break; fi; done; "
              "test -n \"$service\" && ! kill -TERM \"$service\""});
        const auto denied = manager->execute({"/sandbox-tools/agentd", "--run-task", "--", "/bin/true"});
        check(denied.runtime_status == 126 && !denied.timed_out,
              "unprivileged command re-entered privileged task mode");
        exec({"/bin/sh", "-c",
              "test \"$1\" = '--serve' && test \"$2\" = '--run-task' && test \"$3\" = 'a b'",
              "argument-test", "--serve", "--run-task", "a b"});
        check(!fs::exists(info.directory / "guest/rootfs/sandbox-tools/sandbox-task"),
              "VM still deploys a separate task executable");
        manager->write("/project/a", "changed\n");
        check(manager->read("/project/a") == "changed\n", "VM file API failed");
        const std::string binary("a\0b\xff", 4);
        manager->write("/project/binary", binary);
        check(manager->read("/project/binary") == binary, "VM binary file roundtrip failed");
        {
            // Disconnect in the middle of an upload. The privileged service must
            // remove its temporary file using the mapped file identity too.
            namespace agent = protocol;
            const auto until = agent::Clock::now() + std::chrono::seconds(5);
            agent::Channel channel(agent::connect_unix(info.directory / "vm-control/agent.sock", until));
            channel.send({0, agent::Flag::Request, "core.hello",
                          {{"protocol", agent::protocol_name}, {"version", agent::protocol_version},
                           {"file_bytes", 1024}, {"stdin_bytes", 1024}, {"output_bytes", 1024},
                           {"timeout_ms", 2000}}}, until);
            check(channel.receive(until).type == "core.ready", "upload fixture handshake failed");
            channel.send({1, agent::Flag::Request, "fs.write",
                          {{"path", "/project/abandoned"}, {"size", 100}}}, until);
            check(channel.receive(until).type == "fs.write.accepted", "upload fixture rejected");
            channel.send({1, agent::Flag::Event, "fs.write.data",
                          {{"offset", 0}, {"data", agent::binary_bytes("partial")}}}, until);
        }
        check(exec({"/bin/sh", "-c", "find /project -name '.sandbox-io-*'"}).empty(),
              "disconnected VM upload left a temporary file");
        exec({"/bin/test", "!", "-e", "/project/abandoned"});
        rejects([&] { manager->read("/etc/passwd"); });
        rejects([&] { manager->write("/project/../bad", "bad"); });
        exec({"/bin/ln", "-s", "/tmp", "/project/escape"});
        rejects([&] { manager->write("/project/escape/bad", "bad"); });
        exec({"/bin/rm", "/project/escape"});
        CommandRequest input{{"/bin/cat"}, std::nullopt, binary};
        check(manager->execute(input).out == binary, "VM stdin failed");
        // A detached child must remain in the Guest task cgroup and be reclaimed.
        exec({"/bin/sh", "-c", "setsid /bin/sh -c 'sleep 2; echo escaped > /project/escaped' "
                               "</dev/null >/dev/null 2>&1 &"});
        exec({"/bin/sleep", "3"});
        exec({"/bin/test", "!", "-e", "/project/escaped"});
        if (options.environment == Environment::HostTools) {
            manager->write("/project/main.cpp", "#include <iostream>\nint main(){std::cout << 42;}\n");
            exec({"/usr/bin/g++", "/project/main.cpp", "-o", "/build/app"});
            check(exec({"/build/app"}) == "42", "Guest compiler environment failed");
        }
        const auto changes = manager->get_changes();
        check(changes.diff.find("+changed") != std::string::npos, "Guest sync/diff failed");
        check(manager->get_status() == SandboxState::Active, "VM did not resume");
        check(test::read(source / "a") == "original\n", "A changed");
        // Re-open without retaining an in-memory VMM PID or client connection.
        Sandbox reopened(root / "state", make_libkrun_backend());
        reopened.open(info.id);
        check(reopened.read("/project/a") == "changed\n", "VM re-open failed");
        reopened.stop();
        reopened.stop();
        check(reopened.get_status() == SandboxState::Stopped, "VM stop failed");
        check(reopened.get_changes().diff.find("+changed") != std::string::npos,
              "stopped VM lost B");
        reopened.destroy();
        check(!fs::exists(info.directory), "destroy retained B or info artifacts");
        id.clear();
        if (options.environment == Environment::Minimal) {
            options.cmd_timeout = std::chrono::milliseconds(200);
            manager = std::make_unique<Sandbox>(root / "state", make_libkrun_backend());
            const auto timed = manager->create(options);
            id = timed.id;
            const auto result = manager->execute({"/bin/sleep", "10"});
            check(result.timed_out && manager->get_status() == SandboxState::Failed,
                  "VM command timeout did not fail the info");
            check(!fs::exists(root / "state/runtime" / timed.runtime_id), "timeout left VMM running");
            manager->destroy();
            id.clear();

            options.cmd_timeout = std::chrono::seconds(10);
            options.max_output_bytes = 64;
            manager = std::make_unique<Sandbox>(root / "state", make_libkrun_backend());
            const auto bounded = manager->create(options);
            id = bounded.id;
            const auto noisy = manager->execute({"/bin/sh", "-c", "yes x"});
            check(noisy.output_limited && noisy.out.size() + noisy.err.size() <= 64 &&
                  manager->get_status() == SandboxState::Failed, "VM output limit failed");
            check(!fs::exists(root / "state/runtime" / bounded.runtime_id), "output limit left VMM running");
            manager->destroy();
            id.clear();
        }
        std::cout << "real libkrun VM SDK test passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (manager && !id.empty()) {
            try { manager->stop(); } catch (const std::exception &cleanup) {
                std::cerr << "cleanup: " << cleanup.what() << '\n';
            }
        }
        return 1;
    }
}
