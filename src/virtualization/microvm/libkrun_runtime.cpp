#include "virtualization/microvm/libkrun_runtime.hpp"
#include "agent_client.hpp"
#include "ipc/connect.hpp"
#include "lib.hpp"
#include "resources.hpp"
#include "virtualization/bundle.hpp"
#include "virtualization/container/crun_worker_client.hpp"
#include "virtualization/host_tools.hpp"
#include "virtualization/oci.hpp"
#include "virtualization/runtime_files.hpp"
#include "virtualization/runtime_policy.hpp"

#include <fcntl.h>
#include <fstream>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace agent = protocol;
using json = nlohmann::json;

#ifdef SANDBOX_HAS_LIBKRUN
namespace {
    constexpr const char *runner_path = "/sandbox-tools/sandbox-krun";

    void write_json(const fs::path &path, const json &value) {
        std::ofstream out(path);
        out << value.dump(2) << '\n';
        out.close();
        require(bool(out), "cannot write VM configuration: " + path.string());
    }

    SandboxInfo host_runtime_info(const SandboxInfo &info) {
        auto host = info;
        host.bundle_dir = info.directory / "vmm";
        host.options.environment = Environment::Minimal;
        host.options.ctr_repo = "/shares/workspace";
        host.options.cwd_rlt = ".";
        // Host pids count VMM threads; Guest pids independently count Agent tasks.
        host.options.max_tasks = std::max<size_t>(128, info.options.max_tasks);
        return host;
    }

    class LibkrunBackend final : public RuntimeBackend {
      public:
        explicit LibkrunBackend(LibkrunConfig config) : config_(config) {
            require(config.vcpus >= 1 && config.vcpus <= 16 &&
                    config.startup_timeout >= std::chrono::seconds(1) &&
                    config.startup_timeout <= std::chrono::seconds(60), "invalid libkrun configuration");
        }

        std::string id() const override {
            return "vm-libkrun";
        }

        void configure_state_directory(const fs::path &directory) override {
            state_directory_ = directory;
            fs::create_directories(directory);
            guard_ = std::make_unique<CrunWorkerClient>(directory, true);
        }

        void validate_options(const Options &options) override {
            require(geteuid() != 0, "initial libkrun backend requires an ordinary rootless user");
            require(options.memory_bytes >= 256 * 1024 * 1024,
                    "libkrun requires at least 256 MiB including VMM overhead");
            const auto top = *++options.ctr_repo.begin();
            for (const auto *reserved : {"bin", "dev", "proc", "sys", "tmp", "etc", "usr", "lib",
                                        "lib64", "sbin", "build", "env", "cache", "runtime-data",
                                        "sandbox-tools"}) {
                require(top != reserved, "reserved Guest workspace path");
            }
            check_rootless_cgroups(options);
            const int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
            require(kvm >= 0, "libkrun requires ordinary-user access to /dev/kvm");
            const int version = ioctl(kvm, KVM_GET_API_VERSION, 0);
            close(kvm);
            require(version == 12, "unsupported KVM API");
        }

        void prepare(SandboxInfo &info) override {
            require(guard_ != nullptr, "runtime state directory is not configured");
            for (const auto &program : {sandbox_resources::krun_runner_path(),
                                        sandbox_resources::guest_agent_path()}) {
                require(fs::is_regular_file(program) && access(program.c_str(), X_OK) == 0,
                        "SDK VM resource missing or not executable: " + program.string());
            }
            require(fs::is_regular_file(sandbox_resources::krun_firmware_path()),
                    "libkrunfw is unavailable at its configured installation path");
            if (info.options.environment == Environment::HostTools) {
                validate_host_tools(info.options.src_repo, state_directory_.parent_path());
            }
            info.rootless = true;
            // Reuse the environment builder; the OCI config here is not launched.
            auto guest = info;
            guest.bundle_dir = info.directory / "guest";
            prepare_bundle(guest);
            fs::remove(guest.bundle_dir / "config.json");
            const auto guest_root = guest.bundle_dir / "rootfs";
            for (const auto *dir : {"sys", "sys/fs/cgroup", "runtime-data", "build", "env", "cache"}) {
                fs::create_directories(guest_root / dir);
            }
            for (const auto *dir : {"build/tmp", "env/home", "cache/pip"}) {
                fs::create_directories(info.directory / "runtime-data" / dir);
            }
            const auto &o = info.options;
            // RAM is half the Host budget; the remaining half covers the VMM and shared caches.
            const auto ram_mib = o.memory_bytes / (2 * 1024 * 1024);
            require(ram_mib <= UINT32_MAX, "Guest RAM exceeds libkrun limits");
            json spec{{"workspace", o.ctr_repo.string()}, {"max_tasks", o.max_tasks},
                      {"file_bytes", o.max_file_bytes}, {"output_bytes", o.max_output_bytes},
                      {"timeout_ms", o.cmd_timeout.count()}, {"ram_mib", ram_mib},
                      {"vcpus", config_.vcpus}};
            write_json(guest_root / "sandbox-tools/guest.json", spec);

            const auto control = control_directory(info);
            require((control / "agent.sock").string().size() < sizeof(sockaddr_un::sun_path),
                    "manager directory is too long for the VM Unix socket; choose a shorter root");
            fs::create_directory(control);
            fs::permissions(control, fs::perms::owner_all);
            auto host = host_runtime_info(info);
            const auto host_root = host.bundle_dir / "rootfs";
            for (const auto *dir : {"proc", "dev", "dev/pts", "dev/shm", "tmp", "shares/workspace",
                                    "shares/data", "guest-root", "control", "sandbox-tools"}) {
                fs::create_directories(host_root / dir);
            }
            install_runtime_program(host_root, sandbox_resources::krun_runner_path(), runner_path);
            write_json(host_root / "sandbox-tools/krun.json", spec);
            prepare_oci_config(host);
            std::ifstream input(host.bundle_dir / "config.json");
            auto oci = json::parse(input);
            auto bind = [&](const fs::path &source, const fs::path &target, bool readonly,
                            bool nodev = true) {
                json flags = {"bind", readonly ? "ro" : "rw", "nosuid", "private"};
                if (nodev) {
                    flags.push_back("nodev");
                }
                oci["mounts"].push_back({{"source", source.string()}, {"destination", target.string()},
                                         {"type", "bind"}, {"options", flags}});
            };
            bind(guest_root, "/guest-root", true);
            if (o.environment == Environment::HostTools) {
                for (const auto &tools : host_tool_mounts()) {
                    bind(tools.source, fs::path("/guest-root") / tools.destination.relative_path(), true);
                }
            }
            bind(info.directory / "runtime-data", "/shares/data", false);
            bind(control, "/control", false);
            bind("/dev/kvm", "/dev/kvm", false, false);
            oci["process"]["args"] = {runner_path, "/sandbox-tools/krun.json"};
            oci["process"]["cwd"] = "/";
            oci["process"]["env"] = {"PATH=/bin", "LANG=C", "HOME=/",
                                      "LD_LIBRARY_PATH=/sandbox-tools/lib"};
            oci["process"]["rlimits"][0]["soft"] = 1024;
            oci["process"]["rlimits"][0]["hard"] = 1024;
            write_json(host.bundle_dir / "config.json", oci);
        }

        void start(SandboxInfo &info) override {
            checked(guard_->start(info.runtime_id, host_runtime_info(info).bundle_dir.string()));
            const auto state = guard_state(info);
            require(state.at("status") == "running", "VMM worker did not start");
            verify_runtime_resources(host_runtime_info(info), state);
            auto client = connect(info, config_.startup_timeout, true);
            const auto &guest_info = client->runtime_info();
            require(guest_info.at("isolation") == "libkrun" && guest_info.at("task_uid") == 65534 &&
                    guest_info.at("task_pids") == info.options.max_tasks && guest_info.at("network") == "disabled",
                    "Guest resource/identity policy not confirmed");
            info.resource_limits_verified = true;
        }

        RuntimeStatus status(const SandboxInfo &info) override {
            if (!paused_) {
                if (const auto agent = connection_.current()) {
                    try {
                        agent->ping();
                        return {RuntimeState::Running, true, "running", {}};
                    } catch (const std::exception &error) {
                        return {RuntimeState::Unknown, false, "unresponsive", error.what()};
                    }
                }
            }
            const auto response = guard_->state(info.runtime_id);
            if (response.runtime_status != 0 || response.timed_out || response.output_limited) {
                if (!response.timed_out && !response.output_limited &&
                    !fs::exists(state_directory_ / info.runtime_id)) {
                    return {RuntimeState::Missing, true, "missing", response.err};
                }
                return {RuntimeState::Unknown, false, "unknown", response.err};
            }
            try {
                const auto value = json::parse(response.out).at("status").get<std::string>();
                if (value == "paused") {
                    return {RuntimeState::Paused, true, value, {}};
                }
                if (value == "stopped") {
                    return {RuntimeState::Stopped, true, value, {}};
                }
                if (value == "running") {
                    connect(info)->ping();
                    return {RuntimeState::Running, true, value, {}};
                }
                return {RuntimeState::Unknown, false, value, "unknown VMM state"};
            } catch (const std::exception &error) {
                return {RuntimeState::Unknown, false, "unresponsive", error.what()};
            }
        }

        Result execute(const SandboxInfo &info, const RuntimeCommand &command) override {
            return operation(info, [&](agent::Client &client) { return client.execute(command); });
        }
        bool cancel() override {
            const auto client = connection_.current();
            return client && client->cancel();
        }
        Result read(const SandboxInfo &info, const fs::path &path, size_t limit) override {
            return operation(info, [&](agent::Client &client) {
                Result result;
                result.out = client.read(path, limit);
                result.runtime_status = 0;
                return result;
            });
        }
        Result write(const SandboxInfo &info, const fs::path &path, std::string_view content) override {
            return operation(info, [&](agent::Client &client) {
                client.write(path, content);
                Result result;
                result.runtime_status = 0;
                return result;
            });
        }

        void pause(const SandboxInfo &info) override {
            connect(info)->freeze_workspace(true);
            try {
                checked(guard_->pause(info.runtime_id));
                paused_ = true;
            } catch (...) {
                connect(info)->freeze_workspace(false);
                throw;
            }
        }
        void resume(const SandboxInfo &info) override {
            checked(guard_->resume(info.runtime_id));
            connect(info)->freeze_workspace(false);
            paused_ = false;
        }
        void synchronize_workspace(const SandboxInfo &info) override {
            // B is the dedicated virtiofs share. Guest syncfs precedes VM freezing.
            const auto current = status(info);
            if (current.verified && (current.state == RuntimeState::Missing ||
                                     current.state == RuntimeState::Stopped)) {
                return;
            }
            require(current.verified && current.state == RuntimeState::Paused,
                    "workspace inspection requires a paused VM");
        }
        void stop(const SandboxInfo &info) override {
            const auto state = guard_->state(info.runtime_id);
            RuntimeStatus current;
            if (state.runtime_status != 0 && !state.timed_out && !state.output_limited &&
                !fs::exists(state_directory_ / info.runtime_id)) {
                current = {RuntimeState::Missing, true, "missing", {}};
            } else {
                checked(state);
                const auto value = json::parse(state.out).at("status").get<std::string>();
                current = {value == "paused" ? RuntimeState::Paused :
                           value == "running" ? RuntimeState::Running : RuntimeState::Stopped, true, value, {}};
            }
            if (current.state == RuntimeState::Missing && current.verified) {
                remove_control(info);
                return;
            }
            std::string sync_error;
            if (current.state == RuntimeState::Paused) {
                checked(guard_->resume(info.runtime_id));
            }
            if (current.state == RuntimeState::Running || current.state == RuntimeState::Paused) {
                try {
                    connect(info)->freeze_workspace(true);
                } catch (const std::exception &error) {
                    sync_error = error.what();
                }
            }
            require(guard_->destroy(info.runtime_id), "VMM cleanup failed; info artifacts retained");
            remove_control(info);
            require(sync_error.empty(), "VMM reclaimed, but Guest sync failed: " + sync_error);
        }

      private:
        fs::path control_directory(const SandboxInfo &info) const {
            return info.directory / "vm-control";
        }
        void remove_control(const SandboxInfo &info) {
            connection_.clear();
            const auto console = control_directory(info) / "console.log";
            if (fs::exists(console)) {
                fs::rename(console, info.directory / "vm-console.log");
            }
            fs::remove_all(control_directory(info));
        }
        json guard_state(const SandboxInfo &info) {
            const auto result = guard_->state(info.runtime_id);
            checked(result);
            auto state = json::parse(result.out);
            require(state.at("id") == info.runtime_id, "VMM state identity mismatch");
            return state;
        }
        std::shared_ptr<agent::Client> connect(const SandboxInfo &info,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(5),
                                             bool startup = false) {
            agent::Limits limits{info.options.max_file_bytes, info.options.max_file_bytes,
                                 info.options.max_output_bytes, uint32_t(info.options.cmd_timeout.count())};
            return connection_.connect(control_directory(info) / "agent.sock", limits,
                                       "libkrun", timeout, startup);
        }
        template <class F> Result operation(const SandboxInfo &info, F function) {
            try {
                return function(*connect(info));
            } catch (const agent::RemoteError &error) {
                Result result;
                result.runtime_status = 1;
                result.err = error.what();
                return result;
            } catch (const agent::Timeout &error) {
                Result result;
                result.timed_out = true;
                result.err = error.what();
                return result;
            }
        }
        agent::AgentConnection connection_;
        bool paused_ = false;
        LibkrunConfig config_;
        fs::path state_directory_;
        std::unique_ptr<CrunWorkerClient> guard_;
    };
}
#endif

std::unique_ptr<RuntimeBackend> make_libkrun_backend(LibkrunConfig config) {
#ifdef SANDBOX_HAS_LIBKRUN
    return std::make_unique<LibkrunBackend>(config);
#else
    (void)config;
    throw std::runtime_error("libkrun backend disabled; configure -DSANDBOX_ENABLE_LIBKRUN=ON");
#endif
}
