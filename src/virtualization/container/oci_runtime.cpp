#include "lib.hpp"
#include "resources.hpp"
#include "sandbox_types.hpp"
#include "communication/agent_connection.hpp"
#include "virtualization/bundle.hpp"
#include "virtualization/container/crun_worker_client.hpp"
#include "virtualization/host_tools.hpp"
#include "virtualization/runtime.hpp"
#include "virtualization/runtime_policy.hpp"

#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    class Listener {
      public:
        explicit Listener(const fs::path &path) {
            require(path.string().size() < sizeof(sockaddr_un::sun_path),
                    "sandbox directory is too long for its control socket");
            fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            require(fd_ >= 0, "create container listener failed");
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::strcpy(address.sun_path, path.c_str());
            if (bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
                listen(fd_, 4) < 0) {
                close(fd_);
                throw std::runtime_error("bind/listen container control failed");
            }
        }
        ~Listener() { close(fd_); }
        int fd() const { return fd_; }
        Listener(const Listener &) = delete;
        Listener &operator=(const Listener &) = delete;
      private:
        int fd_ = -1;
    };

    class OciCrunBackend final : public RuntimeBackend {
      public:
        std::string id() const override { return "oci-crun"; }

        void configure_state_directory(const fs::path &directory) override {
            state_directory_ = directory;
            fs::create_directories(directory);
            client_ = std::make_unique<CrunWorkerClient>(directory, geteuid() != 0);
        }

        void validate_options(const Options &options) override {
            const auto top = *++options.ctr_repo.begin();
            for (const auto &reserved : {"bin", "dev", "proc", "sys", "tmp", "etc", "usr", "lib",
                                         "sbin", "lib64", "build", "env", "cache", "sandbox-tools"})
                require(top != reserved, "reserved workspace mount path");
            if (geteuid() != 0) check_rootless_cgroups(options);
        }

        void prepare(SandboxInfo &info) override {
            require(client_ != nullptr, "runtime state directory is not configured");
            const auto runner = sandbox_resources::runtime_runner_path();
            require(fs::is_regular_file(runner) && access(runner.c_str(), X_OK) == 0,
                    "SDK runtime runner missing or not executable: " + runner.string());
            if (info.options.environment == Environment::HostTools)
                validate_host_tools(info.options.src_repo, state_directory_.parent_path());
            info.rootless = geteuid() != 0;
            prepare_bundle(info);
            fs::create_directory(control_directory(info));
            fs::permissions(control_directory(info), fs::perms::owner_all);

            std::ifstream input(info.bundle_dir / "config.json");
            auto spec = json::parse(input);
            const auto environment = spec.at("process").at("env");
            spec["process"]["args"] = {"/sandbox-tools/agentd", "--serve", "--workspace",
                                       info.options.ctr_repo.string(), "--listen-fd", "3",
                                       "--container-config", "/sandbox-tools/container.json"};
            std::ofstream config(info.bundle_dir / "config.json");
            config << spec.dump(2);
            config.close();
            require(bool(config), "cannot save agent OCI configuration");
            std::ofstream settings(info.bundle_dir / "rootfs/sandbox-tools/container.json");
            settings << json{{"workspace", info.options.ctr_repo.string()},
                             {"file_bytes", info.options.max_file_bytes},
                             {"output_bytes", info.options.max_output_bytes},
                             {"timeout_ms", info.options.cmd_timeout.count()},
                             {"environment", environment}}.dump(2);
            settings.close();
            require(bool(settings), "cannot save container agent configuration");
        }

        void start(SandboxInfo &info) override {
            // This listener's pathname remains outside the container mount namespace.
            // Only PID 1 receives the open listener; task children close all extra FDs.
            Listener listener(control_directory(info) / "agent.sock");
            checked(client_->start(info.runtime_id, info.bundle_dir.string(), listener.fd()));
            const auto result = client_->state(info.runtime_id);
            checked(result);
            const auto state = json::parse(result.out);
            require(state.at("id") == info.runtime_id && state.at("status") == "running",
                    "runtime failed to enter running state");
            verify_runtime_resources(info, state);
            connect(info, true);
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
            const auto result = client_->state(info.runtime_id);
            if (result.runtime_status != 0 || result.timed_out || result.output_limited) {
                if (!result.timed_out && !result.output_limited &&
                    !fs::exists(state_directory_ / info.runtime_id))
                    return {RuntimeState::Missing, true, "missing", result.err};
                return {RuntimeState::Unknown, false, "unknown", result.err};
            }
            try {
                const auto state = json::parse(result.out);
                require(state.at("id") == info.runtime_id, "runtime returned a different sandbox ID");
                const auto value = state.at("status").get<std::string>();
                if (value == "running") {
                    connect(info)->ping();
                    return {RuntimeState::Running, true, value, {}};
                }
                if (value == "paused") return {RuntimeState::Paused, true, value, {}};
                if (value == "stopped") return {RuntimeState::Stopped, true, value, {}};
                return {RuntimeState::Unknown, false, value, {}};
            } catch (const std::exception &error) {
                return {RuntimeState::Unknown, false, "unresponsive", error.what()};
            }
        }

        Result execute(const SandboxInfo &info, const RuntimeCommand &command) override {
            return operation(info, [&](protocol::Client &agent) { return agent.execute(command); });
        }
        Result read(const SandboxInfo &info, const fs::path &path, size_t limit) override {
            return operation(info, [&](protocol::Client &agent) {
                Result result;
                result.out = agent.read(path, limit);
                result.runtime_status = 0;
                return result;
            });
        }
        Result write(const SandboxInfo &info, const fs::path &path, std::string_view content) override {
            return operation(info, [&](protocol::Client &agent) {
                agent.write(path, content);
                Result result;
                result.runtime_status = 0;
                return result;
            });
        }
        bool cancel() override {
            const auto agent = connection_.current();
            return agent && agent->cancel();
        }
        void pause(const SandboxInfo &info) override {
            checked(client_->pause(info.runtime_id));
            paused_ = true;
        }
        void resume(const SandboxInfo &info) override {
            checked(client_->resume(info.runtime_id));
            paused_ = false;
        }
        void synchronize_workspace(const SandboxInfo &) override {
            // B is directly mounted; it needs no Guest-to-Host synchronization.
        }

        void stop(const SandboxInfo &info) override {
            // Reclamation is independent of agentd, including a broken RPC connection.
            const auto result = client_->state(info.runtime_id);
            if (result.runtime_status != 0 && !result.timed_out && !result.output_limited &&
                !fs::exists(state_directory_ / info.runtime_id)) {
                connection_.clear();
                fs::remove_all(control_directory(info));
                return;
            }
            require(client_->destroy(info.runtime_id), "runtime cleanup failed; sandbox retained");
            connection_.clear();
            fs::remove_all(control_directory(info));
        }

      private:
        fs::path control_directory(const SandboxInfo &info) const {
            return info.directory / "container-control";
        }
        std::shared_ptr<protocol::Client> connect(const SandboxInfo &info, bool startup = false) {
            protocol::Limits limits{info.options.max_file_bytes, info.options.max_file_bytes,
                                    info.options.max_output_bytes, uint32_t(info.options.cmd_timeout.count())};
            return connection_.connect(control_directory(info) / "agent.sock", limits,
                                       "oci-crun", std::chrono::seconds(5), startup);
        }
        template <class F> Result operation(const SandboxInfo &info, F function) {
            try { return function(*connect(info)); }
            catch (const protocol::RemoteError &error) {
                Result result;
                result.runtime_status = 1;
                result.err = error.what();
                return result;
            }
            catch (const protocol::Timeout &error) {
                Result result;
                result.timed_out = true;
                result.err = error.what();
                return result;
            }
        }
        fs::path state_directory_;
        std::unique_ptr<CrunWorkerClient> client_;
        protocol::AgentConnection connection_;
        bool paused_ = false;
    };
}

std::unique_ptr<RuntimeBackend> make_libcrun_backend() { return std::make_unique<OciCrunBackend>(); }
