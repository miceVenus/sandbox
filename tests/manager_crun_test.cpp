#include "../include/sandbox.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;
using test::check;
using test::read;
using test::rejects;
using test::trim;
using test::write;

namespace {
    // Always stop the runtime before deleting the temporary mounted workspace.
    struct SandboxCleanup {
        Sandbox &manager;
        std::string id;
        ~SandboxCleanup() {
            if (!id.empty()) {
                try {
                    manager.stop();
                } catch (const std::exception &error) {
                    std::cerr << "test cleanup failed: " << error.what() << '\n';
                }
            }
        }
    };

    std::string status_field(const std::string &status, const std::string &name) {
        const auto start = status.find(name + ":");
        check(start != std::string::npos, "missing process status field: " + name);
        const auto value = start + name.size() + 1;
        std::istringstream in(status.substr(value, status.find('\n', value) - value));
        std::string result;
        in >> result;
        return result;
    }

    void create_source(const fs::path &source) {
        fs::create_directories(source / "src");
        test::git(source, {"init", "-q"});
        write(source / "src/a", "original\n");
        write(source / "src/remove", "delete me\n");
        write(source / "main.cpp",
              "#include <iostream>\nint main() { std::cout << \"compiled\\n\"; }\n");
        write(source / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.16)\nproject(hello LANGUAGES CXX)\n"
              "enable_testing()\nadd_executable(hello main.cpp)\nadd_test(NAME hello COMMAND "
              "hello)\n");
        const auto wheel = run_process({"/usr/bin/python3",
                                        "-c",
                                        R"PY(
import sys,zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as wheel:
    wheel.writestr('sandbox_probe/__init__.py', 'VALUE = "sandbox-only"\n')
    wheel.writestr('sandbox_probe-1.0.dist-info/METADATA',
                   'Metadata-Version: 2.1\nName: sandbox-probe\nVersion: 1.0\n')
    wheel.writestr('sandbox_probe-1.0.dist-info/WHEEL',
                   'Wheel-Version: 1.0\nGenerator: sandbox-test\n'
                   'Root-Is-Purelib: true\nTag: py3-none-any\n')
    wheel.writestr('sandbox_probe-1.0.dist-info/RECORD', '')
)PY",
                                        (source / "sandbox_probe-1.0-py3-none-any.whl").string()},
                                       10000);
        check(wheel.runtime_status == 0, "cannot create offline pip fixture: " + wheel.err);
        test::commit(source, "base");
    }
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc == 2, "test requires an environment profile");
        test::TemporaryDirectory temp;
        const auto source = temp.path / "source";
        create_source(source);
        const auto root = temp.path / "manager";
        Sandbox manager(root);
        Options options;
        options.src_repo = source / "src";
        options.ctr_repo = "/project";
        options.cwd_rlt = "src";
        options.cmd_timeout = std::chrono::seconds(10);
        options.environment =
            std::string(argv[1]) == "minimal" ? Environment::Minimal : Environment::HostTools;
        const bool rootless = geteuid() != 0;
        const auto user_cgroup =
            fs::path("/sys/fs/cgroup/user.slice") /
            ("user-" + std::to_string(geteuid()) + ".slice") /
            ("user@" + std::to_string(geteuid()) + ".service/cgroup.controllers");
        if (rootless && fs::exists(user_cgroup) &&
            read(user_cgroup).find("cpu") == std::string::npos) {
            rejects([&] { manager.create(options); });
            check(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 1,
                  "delegation failure created info artifacts");
            options.cpu_quota_us = 0;
            std::cout << "Testing explicit CPU quota disabled; CPU is not delegated.\n";
        }
        const auto info = manager.create(options);
        SandboxCleanup cleanup{manager, info.id};
        const auto &id = info.id;
        check(info.options.src_repo == source && info.runtime_backend == "oci-crun" &&
                  info.workspace_backend == "git" && info.rootless == rootless &&
                  info.resource_limits_verified,
              "incorrect SDK info metadata");

        const auto config = json::parse(read(info.bundle_dir / "config.json"));
        check(config["root"]["readonly"] && config["process"]["noNewPrivileges"],
              "unsafe OCI process");
        for (const auto &capabilities : config["process"]["capabilities"]) {
            check(capabilities.empty(), "OCI capabilities not empty");
        }
        size_t workspace_mounts = 0, bind_count = 0;
        std::set<std::string> imports, writable;
        for (const auto &mount : config["mounts"]) {
            if (mount["type"] != "bind") {
                continue;
            }
            ++bind_count;
            const auto destination = mount["destination"].get<std::string>();
            if (destination == "/project") {
                ++workspace_mounts;
                check(mount["source"] == info.work_files_dir.string(), "mounted A instead of B");
            }
            const auto flags = mount["options"].get<std::vector<std::string>>();
            if (std::find(flags.begin(), flags.end(), "rw") != flags.end()) {
                writable.insert(destination);
            }
            if (std::find(flags.begin(), flags.end(), "ro") != flags.end()) {
                imports.insert(destination);
                for (const auto flag : {"bind", "private", "nosuid", "nodev"}) {
                    check(std::find(flags.begin(), flags.end(), flag) != flags.end(),
                          "unsafe host import");
                }
                check(std::find(flags.begin(), flags.end(), "rbind") == flags.end(),
                      "recursive host import");
                const auto path = mount["source"].get<std::string>();
                for (const auto prefix : {"/home", "/root", "/run", "/usr/local"}) {
                    check(path.rfind(prefix, 0) != 0, "private host directory imported");
                }
            }
        }
        check(workspace_mounts == 1, "wrong workspace mount count");
        if (options.environment == Environment::Minimal) {
            check(bind_count == 1, "minimal profile imported host tools");
        } else {
            for (const auto required : {"/usr/bin", "/usr/lib", "/usr/include"}) {
                check(imports.count(required), "required host tool mount missing");
            }
            check(writable == std::set<std::string>{"/project", "/build", "/env", "/cache"},
                  "unexpected writable bind mount");
        }
        if (rootless) {
            check(config["linux"]["uidMappings"] ==
                      json::array({{{"containerID", 0}, {"hostID", geteuid()}, {"size", 1}}}),
                  "wrong UID mapping");
            check(config["linux"]["gidMappings"] ==
                      json::array({{{"containerID", 0}, {"hostID", getegid()}, {"size", 1}}}),
                  "wrong GID mapping");
        }
        if (options.cpu_quota_us == 0) {
            check(!config["linux"]["resources"].contains("cpu"), "disabled CPU quota was emitted");
        }

        // This is an OCI-specific integration test: inspect libcrun's private state,
        // without depending on the external crun executable or widening the public API.
        const auto native = json::parse(read(root / "runtime" / info.runtime_id / "status"));
        const auto proc = fs::path("/proc") / std::to_string(native["pid"].get<int>());
        const auto status = read(proc / "status");
        check(std::stoull(status_field(status, "CapEff"), nullptr, 16) == 0 &&
                  status_field(status, "NoNewPrivs") == "1",
              "native process privilege mismatch");
        if (rootless) {
            check(status_field(status, "Uid") == std::to_string(geteuid()), "wrong host UID");
        }
        for (const auto ns : {"pid", "net", "ipc", "mnt"}) {
            check(fs::read_symlink(proc / "ns" / ns) !=
                      fs::read_symlink(fs::path("/proc/self/ns") / ns),
                  "namespace was shared with host");
        }
        const auto group_record = trim(read(proc / "cgroup"));
        check(group_record.rfind("0::/", 0) == 0, "unexpected cgroup layout");
        const auto cgroup =
            fs::path("/sys/fs/cgroup") / fs::path(group_record.substr(3)).relative_path();
        check(trim(read(cgroup / "memory.max")) == std::to_string(options.memory_bytes) &&
                  trim(read(cgroup / "memory.swap.max")) == "0" &&
                  trim(read(cgroup / "pids.max")) == std::to_string(options.max_tasks),
              "resource limits not applied");
        if (options.cpu_quota_us) {
            check(trim(read(cgroup / "cpu.max")) ==
                      std::to_string(options.cpu_quota_us) + " 100000",
                  "CPU quota not applied");
        }

        auto exec = [&](std::vector<std::string> command, int expected = 0) {
            const auto result = manager.execute(command);
            check(
                !result.timed_out && !result.output_limited &&
                    (expected < 0 ? result.runtime_status != 0 : result.runtime_status == expected),
                "SDK exec failed: " + command[0] + " " + result.err);
            return result.out;
        };
        const auto environment = exec({"/bin/env"});
        std::set<std::string> actual_environment;
        std::istringstream environment_lines(environment);
        for (std::string line; std::getline(environment_lines, line);) {
            actual_environment.insert(line);
        }
        std::set<std::string> expected_environment{"LANG=C",
                                                   "GIT_CONFIG_NOSYSTEM=1",
                                                   "GIT_CONFIG_GLOBAL=/dev/null",
                                                   "GIT_TERMINAL_PROMPT=0"};
        if (options.environment == Environment::HostTools) {
            expected_environment.insert({"PATH=/env/python/bin:/usr/bin:/bin",
                                         "HOME=/env/home",
                                         "TMPDIR=/build/tmp",
                                         "XDG_CACHE_HOME=/cache",
                                         "PIP_CACHE_DIR=/cache/pip",
                                         "PIP_REQUIRE_VIRTUALENV=true"});
        } else {
            expected_environment.insert({"PATH=/bin:/usr/bin", "HOME=/project"});
        }
        check(actual_environment == expected_environment, "unexpected task environment");
        exec({"/bin/sh", "-c", "exit 7"}, 7);
        check(manager.get_status() == SandboxState::Active, "nonzero exit closed info");
        if (options.environment == Environment::HostTools) {
            exec({"/usr/bin/cmake", "-S", "/project", "-B", "/build", "-G", "Ninja"});
            exec({"/usr/bin/cmake", "--build", "/build"});
            check(exec({"/build/hello"}) == "compiled\n", "C++ compile failed");
            exec({"/usr/bin/ctest", "--test-dir", "/build", "--output-on-failure"});
            exec({"/usr/bin/python3", "-m", "venv", "/env/python"});
            exec({"/env/python/bin/python", "-m", "pip", "--version"});
            exec({"/env/python/bin/python",
                  "-m",
                  "pip",
                  "install",
                  "--no-index",
                  "--disable-pip-version-check",
                  "/project/sandbox_probe-1.0-py3-none-any.whl"});
            const auto package = exec({"/env/python/bin/python",
                                       "-c",
                                       "import sandbox_probe; print(sandbox_probe.VALUE); "
                                       "print(sandbox_probe.__file__)"});
            check(package.rfind("sandbox-only\n/env/python/", 0) == 0, "pip escaped info venv");
            exec({"/usr/bin/python3", "-c", "import sandbox_probe"}, -1);
            exec({"/bin/sh",
                  "-c",
                  "echo info-cache > /cache/probe; echo info-home > /env/home/probe"});
            check(read(info.directory / "runtime-data/cache/probe") == "info-cache\n" &&
                      read(info.directory / "runtime-data/env/home/probe") == "info-home\n",
                  "info tool data missing");
            exec({"/bin/sh", "-c", "echo bad > /usr/bin/sdk-host-write"}, -1);
            check(!fs::exists("/usr/bin/sdk-host-write"), "host tools were writable");
            const auto account = getpwuid(geteuid());
            check(account != nullptr, "cannot determine host user home");
            for (const fs::path &path : {source / "src/a",
                                         fs::path(account->pw_dir),
                                         fs::path("/root"),
                                         fs::path("/run"),
                                         fs::path("/usr/local"),
                                         info.directory / "sandbox.json"}) {
                exec({"/bin/test", "!", "-e", path.string()});
            }
            exec({"/bin/ln", "-s", (source / "src/a").string(), "/project/host-link"});
            exec({"/bin/cat", "/project/host-link"}, -1);
            exec({"/bin/rm", "/project/host-link"});
        }
        const auto executed_status = exec({"/bin/cat", "/proc/self/status"});
        check(std::stoull(status_field(executed_status, "CapEff"), nullptr, 16) == 0 &&
                  status_field(executed_status, "NoNewPrivs") == "1",
              "exec gained privileges");
        check(manager.read("/project/src/a") == "original\n", "SDK read failed");
        std::string payload(2 * 1024 * 1024, '\0');
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = char(i % 256);
        }
        const auto unusual = "/project/src/a name; $(touch PWNED)";
        manager.write(unusual, payload);
        check(manager.read(unusual) == payload, "binary roundtrip failed");
        manager.write(unusual, {});
        check(manager.read(unusual).empty(), "empty write failed");
        exec({"/bin/test", "!", "-e", "/project/src/PWNED"});
        exec({"/bin/rm", unusual});
        check(manager.read("/project/.git/HEAD") == "ref: refs/heads/agent\n",
              "task Git missing");
        manager.write("/project/.git/description", "agent repository\n");
        exec({"/bin/sh",
              "-c",
              "echo protected > /tmp/guard; ln -s /tmp/guard file-link; "
              "ln -s /tmp dir-link; mkfifo fifo; echo executable > mode-file; chmod 755 mode-file; "
              "echo hard > hard-file; ln hard-file hard-link; "
              "dd if=/dev/zero of=large-file bs=1048576 count=9 2>/dev/null"});
        for (const auto path : {"/etc/passwd",
                                "/project-other/a",
                                "/project/src/../src/a",
                                "src/a",
                                "/project/src/file-link",
                                "/project/src/dir-link/guard",
                                "/project/src/fifo",
                                "/project/src/large-file"}) {
            rejects([&] { manager.read(path); });
        }
        for (const auto path : {"/etc/new",
                                "/project-other/a",
                                "/project/src/../src/a",
                                "src/a",
                                "/project/src/file-link",
                                "/project/src/dir-link/guard",
                                "/project/src/fifo",
                                "/project/src/hard-file",
                                "/project/missing/a"}) {
            rejects([&] { manager.write(path, "bad"); });
        }
        check(exec({"/bin/cat", "/tmp/guard"}) == "protected\n", "symlink target was overwritten");
        manager.write("/project/src/mode-file", "replaced\n");
        check(trim(exec({"/bin/stat", "-c", "%a", "mode-file"})) == "755",
              "atomic write lost file mode");
        rejects(
            [&] { manager.write("/project/src/a", std::string(8 * 1024 * 1024 + 1, 'x')); });
        const auto helper = info.helper_container_path.string();
        exec({"/bin/sh", "-c", "printf 123456789 | " + helper + " write /project /project/src/a 4"},
             2);
        check(manager.read("/project/src/a") == "original\n", "rejected write damaged target");
        check(exec({"/bin/sh", "-c", "find /project -name '.sandbox-io-*'"}).empty(),
              "atomic write left temporary files");
        exec({"/bin/rm",
              "file-link",
              "dir-link",
              "fifo",
              "mode-file",
              "hard-file",
              "hard-link",
              "large-file"});
        exec({"/bin/sh", "-c", "echo bad > " + helper}, -1);
        exec({"/bin/sh", "-c", "echo changed > a; rm remove; echo new > ../new"});
        check(trim(exec({"/usr/bin/git", "-C", "/project", "rev-parse", "HEAD"})) ==
                  info.base_commit,
              "task baseline mismatch");
        exec({"/usr/bin/git", "-C", "/project", "add", "-A"});
        exec({"/usr/bin/git",
              "-C",
              "/project",
              "-c",
              "user.name=Agent",
              "-c",
              "user.email=agent@example.invalid",
              "commit",
              "-m",
              "agent change"});
        check(trim(exec({"/usr/bin/git", "-C", "/project", "rev-parse", "HEAD"})) !=
                  info.base_commit,
              "task commit failed");
        exec({"/bin/sh", "-c", "echo bad > /bin/bad"}, -1);
        exec({"/bin/test", "!", "-e", "/project/manager.git"});
        rejects([&] { manager.execute(CommandRequest{{"/bin/pwd"}, "../outside"}); });
        rejects([&] { manager.execute(CommandRequest{{"/bin/pwd"}, "/tmp"}); });
        exec({"/bin/ln", "-s", "/tmp", "escape"});
        check(manager.execute(CommandRequest{{"/bin/pwd"}, "src/escape"}).out == "/tmp\n",
              "container cwd symlink was resolved on host");
        check(
            manager.execute(CommandRequest{{"/bin/pwd"}, "missing-directory"}).runtime_status !=
                0,
            "missing cwd accepted");
        exec({"/bin/rm", "escape"});
        check(read(source / "src/a") == "original\n" &&
                  read(source / "src/remove") == "delete me\n" && !fs::exists(source / "new"),
              "A was modified");
        const auto changes = manager.get_changes();
        check(changes.diff.find("+changed") != std::string::npos &&
                  changes.diff.find("+new") != std::string::npos &&
                  changes.diff.find("deleted file mode") != std::string::npos &&
                  changes.diff.find("info-cache") == std::string::npos &&
                  changes.diff.find("info-home") == std::string::npos,
              "SDK preview was incomplete");
        check(manager.get_status() == SandboxState::Active, "preview failed to resume");
        check(manager.execute(std::vector<std::string>{"/bin/sleep", "30"}).timed_out,
              "runtime timeout failed");
        check(manager.get_status() == SandboxState::Failed, "timeout failure not persisted");
        rejects([&] { manager.execute(std::vector<std::string>{"/bin/pwd"}); });
        rejects([&] { manager.read("/project/src/a"); });
        rejects([&] { manager.write("/project/src/a", "bad"); });
        check(manager.get_changes().diff.find("+changed") != std::string::npos, "timeout lost B");
        manager.stop();
        manager.stop();
        check(!fs::exists(root / "runtime" / info.runtime_id), "runtime state leaked");
        for (int i = 0; i < 100 && fs::exists(cgroup); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(!fs::exists(cgroup), "container cgroup leaked");
        std::cout << "SDK real container tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
