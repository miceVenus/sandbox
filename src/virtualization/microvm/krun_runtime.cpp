#include "virtualization/microvm/krun_runtime.hpp"
#include "lib/error.hpp"
#include "lib/json.hpp"
#include "lib/process.hpp"
#include "resources.hpp"
#include "virtualization/agentd_client.hpp"
#include "virtualization/environment/environment.hpp"
#include "virtualization/container_client.hpp"
#include "virtualization/container/resources.hpp"

#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

using json = nlohmann::json;
namespace environment = virtualization::environment;

#ifdef SANDBOX_HAS_LIBKRUN
namespace {
    constexpr const char *runner_path = "/sandbox-tools/sandbox-krun";

    auto host_runtime_info(const SandboxInfo &info) -> SandboxInfo {
        auto host = info;
        host.bundle_dir = info.directory / "vmm";
        host.options.environment = Environment::Minimal;
        host.options.ctr_repo = "/shares/workspace";
        host.options.cwd_rlt = ".";
        // Host pids count VMM threads; Guest pids independently count sandbox tasks.
        host.options.max_tasks = std::max<size_t>(128, info.options.max_tasks);
        return host;
    }

    class KrunBackend final : public RuntimeBackend {
      public:
        explicit KrunBackend(KrunConfig config) : config_(config) {
            lib::require(config.vcpus >= 1 && config.vcpus <= 16 &&
                             config.startup_timeout >= std::chrono::seconds(1) &&
                             config.startup_timeout <= std::chrono::seconds(60),
                         "invalid libkrun configuration");
        }

        auto id() const -> std::string override {
            return "vm-libkrun";
        }

        void configure_state_directory(const fs::path &directory) override {
            state_directory_ = directory;
            fs::create_directories(directory);
            container_client_ = std::make_unique<ContainerClient>(directory, true);
        }

        void validate_options(const Options &options) override {
            lib::require(geteuid() != 0,
                         "initial libkrun backend requires an ordinary rootless user");
            lib::require(options.memory_bytes >= 256 * 1024 * 1024,
                         "libkrun requires at least 256 MiB including VMM overhead");
            const auto top = *++options.ctr_repo.begin();
            for (const auto *reserved :
                 {"bin", "dev", "proc", "sys", "tmp", "etc", "usr", "lib", "lib64", "sbin", "build",
                  "env", "cache", "runtime-data", "sandbox-tools"}) {
                lib::require(top != reserved, "reserved Guest workspace path");
            }
            check_rootless_cgroups(options);
            const int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
            lib::require(kvm >= 0, "libkrun requires ordinary-user access to /dev/kvm");
            const int version = ioctl(kvm, KVM_GET_API_VERSION, 0);
            close(kvm);
            lib::require(version == 12, "unsupported KVM API");
        }

        void prepare(SandboxInfo &info) override {
            lib::require(container_client_ != nullptr, "runtime state directory is not configured");
            for (const auto &program :
                 {sandbox_resources::krun_runner_path(), sandbox_resources::agentd_path()}) {
                lib::require(fs::is_regular_file(program) && access(program.c_str(), X_OK) == 0,
                             "SDK VM resource missing or not executable: " + program.string());
            }
            lib::require(fs::is_regular_file(sandbox_resources::krun_firmware_path()),
                         "libkrunfw is unavailable at its configured installation path");
            const auto environment_config = environment::make_config(info.options);
            environment::validate_host_tools(environment_config, info.options.src_repo,
                                              state_directory_.parent_path());
            info.rootless = true;
            // Prepare the Guest userspace independently of the outer OCI container.
            auto guest = info;
            guest.bundle_dir = info.directory / "guest";
            environment::prepare_rootfs(guest, environment_config);
            const auto guest_root = guest.bundle_dir / "rootfs";
            for (const auto *directory : {"sys/fs/cgroup", "runtime-data"}) {
                fs::create_directories(guest_root / directory);
            }
            if (info.options.environment == Environment::Minimal) {
                environment::prepare_runtime_data(guest);
            }

            const auto &o = info.options;
            // RAM is half the Host budget; the remaining half covers the VMM and shared caches.
            const auto ram_mib = o.memory_bytes / (2 * 1024 * 1024);
            lib::require(ram_mib <= UINT32_MAX, "Guest RAM exceeds libkrun limits");
            json spec{{"workspace", o.ctr_repo.string()},
                      {"max_tasks", o.max_tasks},
                      {"environment", environment_config.variables},
                      {"file_bytes", o.max_file_bytes},
                      {"output_bytes", o.max_output_bytes},
                      {"timeout_ms", o.cmd_timeout.count()},
                      {"ram_mib", ram_mib},
                      {"vcpus", config_.vcpus}};
            lib::write_json(guest_root / "sandbox-tools/guest.json", spec);

            const auto control = control_directory(info);
            lib::require(
                (control / "agentd.sock").string().size() < sizeof(sockaddr_un::sun_path),
                "manager directory is too long for the VM Unix socket; choose a shorter root");
            fs::create_directory(control);
            fs::permissions(control, fs::perms::owner_all);
            auto host = host_runtime_info(info);
            const auto host_root = host.bundle_dir / "rootfs";
            for (const auto *dir : {"proc", "dev", "dev/pts", "dev/shm", "tmp", "shares/workspace",
                                    "shares/data", "guest-root", "control", "sandbox-tools"}) {
                fs::create_directories(host_root / dir);
            }
            environment::install_program(host_root, sandbox_resources::krun_runner_path(),
                                         runner_path);
            lib::write_json(host_root / "sandbox-tools/krun.json", spec);
            const environment::Config vmm_environment{
                {}, {"PATH=/bin", "LANG=C", "HOME=/", "LD_LIBRARY_PATH=/sandbox-tools/lib"}};
            auto oci = environment::make_oci_config(host, vmm_environment);
            auto bind = [&](const fs::path &source, const fs::path &target, bool readonly,
                            bool nodev = true) {
                json flags = {"bind", readonly ? "ro" : "rw", "nosuid", "private"};
                if (nodev) {
                    flags.push_back("nodev");
                }
                oci["mounts"].push_back({{"source", source.string()},
                                         {"destination", target.string()},
                                         {"type", "bind"},
                                         {"options", flags}});
            };
            bind(guest_root, "/guest-root", true);
            for (const auto &tools : environment_config.tool_mounts) {
                bind(tools.source, fs::path("/guest-root") / tools.destination.relative_path(), true);
            }
            bind(info.directory / "runtime-data", "/shares/data", false);
            bind(control, "/control", false);
            bind("/dev/kvm", "/dev/kvm", false, false);
            oci["process"]["args"] = {runner_path, "/sandbox-tools/krun.json"};
            oci["process"]["cwd"] = "/";
            oci["process"]["rlimits"][0]["soft"] = 1024;
            oci["process"]["rlimits"][0]["hard"] = 1024;
            lib::write_json(host.bundle_dir / "config.json", oci);
        }

        void start(SandboxInfo &info) override {
            lib::check_process_result(container_client_->start(
                info.runtime_id, host_runtime_info(info).bundle_dir.string()));
            const auto state = container_client_state(info);
            lib::require(state.at("status") == "running", "VMM worker did not start");
            verify_runtime_resources(host_runtime_info(info), state);
            auto client = connect(info, config_.startup_timeout, true);
            const auto &guest_info = client->runtime_info();
            lib::require(guest_info.at("isolation") == "libkrun" &&
                             guest_info.at("task_uid") == 65534 &&
                             guest_info.at("task_pids") == info.options.max_tasks &&
                             guest_info.at("network") == "disabled",
                         "Guest resource/identity policy not confirmed");
            info.resource_limits_verified = true;
        }

        auto status(const SandboxInfo &info) -> RuntimeStatus override {
            if (!paused_) {
                if (agentd_client_.is_connected()) {
                    try {
                        agentd_client_.ping();
                        return {RuntimeState::Running, true, "running", {}};
                    } catch (const std::exception &error) {
                        return {RuntimeState::Unknown, false, "unresponsive", error.what()};
                    }
                }
            }
            const auto response = container_client_->state(info.runtime_id);
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

        auto execute(const RuntimeCommand &command) -> Result override {
            return agentd_client_.execute_result(command);
        }

        auto read(const fs::path &path, size_t limit) -> Result override {
            return agentd_client_.read_result(path, limit);
        }

        auto write(const fs::path &path, std::string_view contents) -> Result override {
            return agentd_client_.write_result(path, contents);
        }

        auto cancel() -> bool override {
            return agentd_client_.cancel();
        }

        void pause(const SandboxInfo &info) override {
            connect(info)->freeze_workspace(true);
            try {
                lib::check_process_result(container_client_->pause(info.runtime_id));
                paused_ = true;
            } catch (...) {
                connect(info)->freeze_workspace(false);
                throw;
            }
        }
        void resume(const SandboxInfo &info) override {
            lib::check_process_result(container_client_->resume(info.runtime_id));
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
            lib::require(current.verified && current.state == RuntimeState::Paused,
                         "workspace inspection requires a paused VM");
        }
        void stop(const SandboxInfo &info) override {
            const auto state = container_client_->state(info.runtime_id);
            RuntimeStatus current;
            if (state.runtime_status != 0 && !state.timed_out && !state.output_limited &&
                !fs::exists(state_directory_ / info.runtime_id)) {
                current = {RuntimeState::Missing, true, "missing", {}};
            } else {
                lib::check_process_result(state);
                const auto value = json::parse(state.out).at("status").get<std::string>();
                current = {value == "paused"    ? RuntimeState::Paused
                           : value == "running" ? RuntimeState::Running
                                                : RuntimeState::Stopped,
                           true,
                           value,
                           {}};
            }
            if (current.state == RuntimeState::Missing && current.verified) {
                remove_control(info);
                return;
            }
            std::string sync_error;
            if (current.state == RuntimeState::Paused) {
                lib::check_process_result(container_client_->resume(info.runtime_id));
            }
            if (current.state == RuntimeState::Running || current.state == RuntimeState::Paused) {
                try {
                    connect(info)->freeze_workspace(true);
                } catch (const std::exception &error) {
                    sync_error = error.what();
                }
            }
            lib::require(container_client_->destroy(info.runtime_id),
                         "VMM cleanup failed; info artifacts retained");
            remove_control(info);
            lib::require(sync_error.empty(), "VMM reclaimed, but Guest sync failed: " + sync_error);
        }

      private:
        auto control_directory(const SandboxInfo &info) const -> fs::path {
            return info.directory / "control";
        }
        void remove_control(const SandboxInfo &info) {
            agentd_client_.disconnect();
            const auto console = control_directory(info) / "console.log";
            if (fs::exists(console)) {
                fs::rename(console, info.directory / "vm-console.log");
            }
            fs::remove_all(control_directory(info));
        }
        auto container_client_state(const SandboxInfo &info) -> json {
            const auto result = container_client_->state(info.runtime_id);
            lib::check_process_result(result);
            auto state = json::parse(result.out);
            lib::require(state.at("id") == info.runtime_id, "VMM state identity mismatch");
            return state;
        }
        auto connect(const SandboxInfo &info,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5),
                     bool startup = false) -> virtualization::AgentdClient * {

            ipc::Limits limits{info.options.max_file_bytes, info.options.max_file_bytes,
                               info.options.max_output_bytes,
                               uint32_t(info.options.cmd_timeout.count())};
            agentd_client_.connect(control_directory(info) / "agentd.sock", limits, "libkrun",
                                    timeout, startup);
            return &agentd_client_;
        }
        virtualization::AgentdClient agentd_client_;
        std::unique_ptr<ContainerClient> container_client_;
        bool paused_ = false;
        KrunConfig config_;
        fs::path state_directory_;
    };
} // namespace
#endif

auto make_krun_backend(KrunConfig config) -> std::unique_ptr<RuntimeBackend> {
#ifdef SANDBOX_HAS_LIBKRUN
    return std::make_unique<KrunBackend>(config);
#else
    (void)config;
    throw std::runtime_error("libkrun backend disabled; configure -DSANDBOX_ENABLE_LIBKRUN=ON");
#endif
}
