#include "agentd/microvm_bootstrap.hpp"
#include "agentd/task_runner.hpp"
#include "lib/descriptor.hpp"

// glibc declares MS_* as enum values; include it before Linux macro definitions.
#include <sys/mount.h>

#include <fcntl.h>
#include <fstream>
#include <linux/mount.h>
#include <sched.h>
#include <signal.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace agentd {
    using namespace ipc;
    namespace {
        void set_control(const std::filesystem::path &path, const std::string &value) {
            std::ofstream out(path);
            out << value;
            out.close();
            require(bool(out), "Guest cgroup control failed: " + path.string());
        }
        void wait_event(const std::filesystem::path &group, const std::string &event) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            do {
                std::ifstream in(group / "cgroup.events");
                const std::string content((std::istreambuf_iterator<char>(in)), {});
                if (content.find(event) != std::string::npos) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < until);
            throw std::runtime_error("Guest cgroup state not confirmed: " + event);
        }
        auto task_idmap() -> lib::UniqueFd {
            int ready[2];
            require(pipe2(ready, O_CLOEXEC) == 0, "Guest idmap pipe failed");
            lib::UniqueFd reader(ready[0]), writer(ready[1]);
            const pid_t child = fork();
            if (child == 0) {
                reader.reset();
                const char result = unshare(CLONE_NEWUSER) == 0 ? 0 : 1;
                const auto ignored = write(writer.get(), &result, 1);
                (void)ignored;
                if (result != 0) {
                    _exit(1);
                }
                for (;;) {
                    pause();
                }
            }
            writer.reset();
            if (child < 0) {
                throw std::runtime_error("Guest idmap fork failed");
            }
            struct Child {
                pid_t pid;
                ~Child() {
                    kill(pid, SIGKILL);
                    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                    }
                }
            } owner{child};
            char result = 1;
            const auto count = read(reader.get(), &result, 1);
            reader.reset();
            require(count == 1 && result == 0, "Guest user namespace unavailable");
            const auto proc = std::filesystem::path("/proc") / std::to_string(child);
            set_control(proc / "uid_map", "0 " + std::to_string(task_uid) + " 1\n");
            set_control(proc / "gid_map", "0 " + std::to_string(task_gid) + " 1\n");
            lib::UniqueFd fd(open((proc / "ns/user").c_str(), O_RDONLY | O_CLOEXEC));
            require(fd.get() >= 0, "open Guest idmap failed");
            return fd;
        }
        void map_task_share(const char *path, int userns) {
            lib::UniqueFd tree(
                syscall(SYS_open_tree, AT_FDCWD, path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC));
            require(tree.get() >= 0, "clone Guest share mount failed");
            mount_attr attr{};
            attr.attr_set = MOUNT_ATTR_IDMAP;
            attr.userns_fd = userns;
            require(syscall(SYS_mount_setattr, tree.get(), "", AT_EMPTY_PATH, &attr, sizeof(attr)) == 0,
                    "Guest virtiofs idmapped mount failed");
            require(syscall(SYS_move_mount, tree.get(), "", AT_FDCWD, path,
                            MOVE_MOUNT_F_EMPTY_PATH) == 0,
                    "move Guest idmapped mount failed");
        }
    } // namespace

    void configure_microvm(ServiceConfig &config, const std::filesystem::path &settings) {
        struct statfs root{};
        require(geteuid() == 0 && statfs("/", &root) == 0 && root.f_type == 0x65735546,
                "VM bootstrap requires a privileged virtiofs Guest");
        std::ifstream input(settings);
        const auto spec = Json::parse(input);
        require(config.workspace == spec.at("workspace").get<std::string>(),
                "Guest workspace configuration mismatch");
        const auto flags = MS_NOSUID | MS_NODEV;
        require(mount("sandbox-workspace", config.workspace.c_str(), "virtiofs", flags, nullptr) == 0,
                "mount Guest workspace failed");
        require(mount("sandbox-data", "/runtime-data", "virtiofs", flags, nullptr) == 0,
                "mount Guest runtime data failed");
        // Host B stays owned by the invoking user (VMM UID 0). Only inside these
        // Guest mounts does that ownership become the unprivileged task UID.
        {
            auto userns = task_idmap();
            map_task_share(config.workspace.c_str(), userns.get());
            map_task_share("/runtime-data", userns.get());
        }
        for (const auto *name : {"build", "env", "cache"}) {
            const auto source = std::string("/runtime-data/") + name;
            const auto target = std::string("/") + name;
            require(mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) == 0,
                    "mount Guest data alias failed");
        }
        require(mount("tmpfs", "/tmp", "tmpfs", flags, "mode=1777,size=16m") == 0,
                "mount Guest temporary directory failed");
        // cgroup subtree lives on the Guest kernel, outside all writable shares.
        const std::filesystem::path task_group = task_cgroup;
        set_control("/sys/fs/cgroup/cgroup.subtree_control", "+pids");
        std::filesystem::create_directory(task_group);
        const auto pids = spec.at("max_tasks").get<size_t>();
        set_control(task_group / "pids.max", std::to_string(pids));
        require(std::filesystem::exists(task_group / "cgroup.kill"),
                "Guest kernel must support cgroup.kill");
        config.task_launcher = "/sandbox-tools/agentd";
        config.mapped_file_identity = true;
        config.cleanup_tasks = [task_group] () -> void {
            set_control(task_group / "cgroup.kill", "1");
            wait_event(task_group, "populated 0");
        };
        config.freeze_workspace = [task_group, workspace = config.workspace](bool frozen) -> void {
            set_control(task_group / "cgroup.freeze", frozen ? "1" : "0");
            wait_event(task_group, frozen ? "frozen 1" : "frozen 0");
            if (frozen) {
                const lib::UniqueFd fd(open(workspace.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                require(fd.get() >= 0, "open Guest workspace for synchronization failed");
                const int result = syncfs(fd.get());
                require(result == 0, "Guest workspace synchronization failed");
            }
        };
        config.task_environment = spec.at("environment").get<std::vector<std::string>>();
        config.limits.file_bytes = spec.at("file_bytes");
        config.limits.stdin_bytes = config.limits.file_bytes;
        config.limits.output_bytes = spec.at("output_bytes");
        config.limits.timeout_ms = spec.at("timeout_ms");
        config.runtime_info = {{"isolation", "libkrun"},
                               {"task_uid", task_uid},
                               {"task_pids", pids},
                               {"network", "disabled"}};
    }
} // namespace agentd
