#include "ipc/socket.hpp"
#include "lib/error.hpp"
#include "lib/json.hpp"
#include "lib/process.hpp"
#include "resources.hpp"
#include "sandbox_types.hpp"
#include "virtualization/agentd_client.hpp"
#include "virtualization/container/resources.hpp"
#include "virtualization/container_client.hpp"
#include "virtualization/environment/environment.hpp"
#include "virtualization/runtime.hpp"

#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace environment = virtualization::environment;

namespace {

    class CrunBackend final : public RuntimeBackend {
      public:
        auto id() const -> std::string override {
            return "oci-crun";
        }

        void configure_state_directory(const fs::path &directory) override {
            state_directory_ = directory;
            fs::create_directories(directory);
            container_client_ = std::make_unique<ContainerClient>(directory, geteuid() != 0);
        }

        void validate_options(const Options &options) override {
            const auto top = *++options.ctr_repo.begin();
            for (const auto &reserved : {"bin", "dev", "proc", "sys", "tmp", "etc", "usr", "lib",
                                         "sbin", "lib64", "build", "env", "cache", "sandbox-tools"})
                lib::require(top != reserved, "reserved workspace mount path");
            if (geteuid() != 0)
                check_rootless_cgroups(options);
        }

        void prepare(SandboxInfo &info) override {
            lib::require(container_client_ != nullptr, "runtime state directory is not configured");

            // runner == 额外编译的临时进程的地址
            const auto runner = sandbox_resources::crun_worker_path();

            lib::require(fs::is_regular_file(runner) && access(runner.c_str(), X_OK) == 0,
                         "SDK runtime runner missing or not executable: " + runner.string());

            const auto environment_config = environment::make_config(info.options);
            environment::validate_host_tools(environment_config, info.options.src_repo,
                                              state_directory_.parent_path());

            info.rootless = geteuid() != 0;

            environment::prepare_rootfs(info, environment_config);

            fs::create_directory(control_directory(info));
            fs::permissions(control_directory(info), fs::perms::owner_all);

            auto spec = environment::make_oci_config(info, environment_config);
            spec["process"]["args"] = {"/sandbox-tools/agentd", "--serve",
                                       "--workspace",           info.options.ctr_repo.string(),
                                       "--listen-fd",           "3",
                                       "--container-config",    "/sandbox-tools/container.json"};
            lib::write_json(info.bundle_dir / "config.json", spec);
            lib::write_json(info.bundle_dir / "rootfs/sandbox-tools/container.json",
                            {{"workspace", info.options.ctr_repo.string()},
                             {"file_bytes", info.options.max_file_bytes},
                             {"output_bytes", info.options.max_output_bytes},
                             {"timeout_ms", info.options.cmd_timeout.count()},
                             {"environment", environment_config.variables}});
        }

        void start(SandboxInfo &info) override {
            // This listener's pathname remains outside the container mount namespace.
            // Only PID 1 receives the open listener; task children close all extra FDs.
            auto listener = ipc::listen_unix(control_directory(info) / "agentd.sock");
            lib::check_process_result(container_client_->start(
                info.runtime_id, info.bundle_dir.string(), listener.get()));
            const auto result = container_client_->state(info.runtime_id);
            lib::check_process_result(result);
            const auto state = json::parse(result.out);
            lib::require(state.at("id") == info.runtime_id && state.at("status") == "running",
                         "runtime failed to enter running state");
            verify_runtime_resources(info, state);
            connect(info, true);
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
            const auto result = container_client_->state(info.runtime_id);
            if (result.runtime_status != 0 || result.timed_out || result.output_limited) {
                if (!result.timed_out && !result.output_limited &&
                    !fs::exists(state_directory_ / info.runtime_id))
                    return {RuntimeState::Missing, true, "missing", result.err};
                return {RuntimeState::Unknown, false, "unknown", result.err};
            }
            try {
                const auto state = json::parse(result.out);
                lib::require(state.at("id") == info.runtime_id,
                             "runtime returned a different sandbox ID");
                const auto value = state.at("status").get<std::string>();
                if (value == "running") {
                    connect(info)->ping();
                    return {RuntimeState::Running, true, value, {}};
                }
                if (value == "paused")
                    return {RuntimeState::Paused, true, value, {}};
                if (value == "stopped")
                    return {RuntimeState::Stopped, true, value, {}};
                return {RuntimeState::Unknown, false, value, {}};
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
            lib::check_process_result(container_client_->pause(info.runtime_id));
            paused_ = true;
        }
        void resume(const SandboxInfo &info) override {
            lib::check_process_result(container_client_->resume(info.runtime_id));
            paused_ = false;
        }

        void synchronize_workspace(const SandboxInfo &) override {
            // B is directly mounted; it needs no Guest-to-Host synchronization.
        }

        void stop(const SandboxInfo &info) override {
            // Reclamation is independent of agentd, including a broken RPC connection.
            const auto result = container_client_->state(info.runtime_id);
            if (result.runtime_status != 0 && !result.timed_out && !result.output_limited &&
                !fs::exists(state_directory_ / info.runtime_id)) {
                agentd_client_.disconnect();
                fs::remove_all(control_directory(info));
                return;
            }
            lib::require(container_client_->destroy(info.runtime_id),
                         "runtime cleanup failed; sandbox retained");
            agentd_client_.disconnect();
            fs::remove_all(control_directory(info));
        }

      private:
        auto control_directory(const SandboxInfo &info) const -> fs::path {
            return info.directory / "control";
        }

        auto connect(const SandboxInfo &info, bool startup = false)
            -> virtualization::AgentdClient * {

            ipc::Limits limits{info.options.max_file_bytes, info.options.max_file_bytes,
                               info.options.max_output_bytes,
                               uint32_t(info.options.cmd_timeout.count())};
            agentd_client_.connect(control_directory(info) / "agentd.sock", limits, "oci-crun",
                                    std::chrono::seconds(5), startup);
            return &agentd_client_;
        }

        fs::path state_directory_;
        std::unique_ptr<ContainerClient> container_client_;
        virtualization::AgentdClient agentd_client_;
        bool paused_ = false;
    };
} // namespace

auto make_crun_backend() -> std::unique_ptr<RuntimeBackend> {
    return std::make_unique<CrunBackend>();
}
