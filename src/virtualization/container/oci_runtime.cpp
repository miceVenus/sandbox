#include "lib.hpp"
#include "resources.hpp"
#include "sandbox_types.hpp"
#include "virtualization/bundle.hpp"
#include "virtualization/container/crun_worker_client.hpp"
#include "virtualization/host_tools.hpp"
#include "virtualization/runtime.hpp"
#include "virtualization/runtime_policy.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    class OciCrunBackend final : public RuntimeBackend {
      public:
        std::string id() const override {
            return "oci-crun";
        }

        void configure_state_directory(const fs::path &directory) override {
            state_directory_ = directory;
            fs::create_directories(directory);
            client_ = std::make_unique<CrunWorkerClient>(directory, geteuid() != 0);
        }

        void validate_options(const Options &options) override {
            const auto top = *++options.ctr_repo.begin();
            for (const auto &reserved : {"bin",
                                         "dev",
                                         "proc",
                                         "sys",
                                         "tmp",
                                         "etc",
                                         "usr",
                                         "lib",
                                         "sbin",
                                         "lib64",
                                         "build",
                                         "env",
                                         "cache",
                                         "sandbox-tools"}) {
                require(top != reserved, "reserved workspace mount path");
            }
            if (geteuid() != 0) {
                check_rootless_cgroups(options);
            }
        }

        void prepare(SandboxInfo &info) override {
            require(client_ != nullptr, "runtime state directory is not configured");
            const auto runner = sandbox_resources::runtime_runner_path();
            require(fs::is_regular_file(runner) && access(runner.c_str(), X_OK) == 0,
                    "SDK runtime runner missing or not executable: " + runner.string());
            if (info.options.environment == Environment::HostTools) {
                validate_host_tools(info.options.src_repo, state_directory_.parent_path());
            }
            info.rootless = geteuid() != 0;
            prepare_bundle(info);
        }

        void start(SandboxInfo &info) override {
            checked(client_->start(info.runtime_id, info.bundle_dir.string()));
            const auto result = client_->state(info.runtime_id);
            checked(result);
            const auto state = json::parse(result.out);
            require(state.at("id") == info.runtime_id && state.at("status") == "running",
                    "runtime failed to enter running state");
            verify_runtime_resources(info, state);
            info.resource_limits_verified = true;
        }

        RuntimeStatus status(const SandboxInfo &info) override {
            const auto result = client_->state(info.runtime_id);
            if (result.runtime_status != 0 || result.timed_out || result.output_limited) {
                if (!result.timed_out && !result.output_limited &&
                    !fs::exists(state_directory_ / info.runtime_id)) {
                    return {RuntimeState::Missing, true, "missing", result.err};
                }
                return {RuntimeState::Unknown, false, "unknown", result.err};
            }
            try {
                const auto state = json::parse(result.out);
                require(state.at("id") == info.runtime_id, "runtime returned a different info ID");
                const auto value = state.at("status").get<std::string>();
                RuntimeState kind = RuntimeState::Unknown;
                if (value == "running") {
                    kind = RuntimeState::Running;
                } else if (value == "paused") {
                    kind = RuntimeState::Paused;
                } else if (value == "stopped") {
                    kind = RuntimeState::Stopped;
                }
                return {kind, kind != RuntimeState::Unknown, value, result.err};
            } catch (const std::exception &error) {
                return {RuntimeState::Unknown, false, "unknown", error.what()};
            }
        }

        Result execute(const SandboxInfo &info, const RuntimeCommand &command) override {
            return client_->exec(info.runtime_id,
                                 command.cwd.string(),
                                 command.argv,
                                 command.timeout_ms,
                                 command.output_limit,
                                 command.stdin_data);
        }

        Result read(const SandboxInfo &info, const fs::path &path, size_t limit) override {
            return file_operation(info, "read", path, {}, limit);
        }

        Result
        write(const SandboxInfo &info, const fs::path &path, std::string_view content) override {
            return file_operation(info, "write", path, content, info.options.max_file_bytes);
        }

        void pause(const SandboxInfo &info) override {
            checked(client_->pause(info.runtime_id));
        }

        void resume(const SandboxInfo &info) override {
            checked(client_->resume(info.runtime_id));
        }

        void synchronize_workspace(const SandboxInfo &) override {
            // OCI directly mounts B. A VM implementation exports its guest workspace here.
        }

        void stop(const SandboxInfo &info) override {
            const auto current = status(info);
            if (current.verified && current.state == RuntimeState::Missing) {
                return;
            }
            require(client_->destroy(info.runtime_id), "runtime cleanup failed; info retained");
        }

      private:
        Result file_operation(const SandboxInfo &info,
                              const std::string &operation,
                              const fs::path &path,
                              std::string_view content,
                              size_t limit) {
            RuntimeCommand command;
            command.argv = {info.helper_container_path.string(),
                            operation,
                            info.options.ctr_repo.string(),
                            path.string(),
                            std::to_string(limit)};
            command.cwd = info.options.ctr_repo;
            command.stdin_data = std::string(content);
            command.timeout_ms = int(info.options.cmd_timeout.count());
            command.output_limit = info.options.max_output_bytes;
            return execute(info, command);
        }

        fs::path state_directory_;
        std::unique_ptr<CrunWorkerClient> client_;
    };
} // namespace

std::unique_ptr<RuntimeBackend> make_libcrun_backend() {
    return std::make_unique<OciCrunBackend>();
}
