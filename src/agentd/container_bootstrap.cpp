#include "agentd/container_bootstrap.hpp"
#include "lib/process.hpp"

#include <csignal>
#include <fstream>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace agentd {
    using namespace ipc;
    namespace {
        void finish_container_tasks() {
            require(getpid() == 1, "container task reclamation requires PID 1");
            const auto until = Clock::now() + std::chrono::seconds(3);
            for (;;) {
                bool found = false;
                for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
                    const auto name = entry.path().filename().string();
                    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
                        continue;
                    }
                    const auto pid = std::stol(name);
                    if (pid <= 1) {
                        continue;
                    }
                    found = true;
                    kill(static_cast<pid_t>(pid), SIGKILL);
                }
                // lib::run_process has reaped its direct child. PID 1 also owns orphaned
                // descendants, including those which escaped a task process group.
                while (waitpid(-1, nullptr, WNOHANG) > 0) {
                }
                if (!found) {
                    return;
                }
                require(Clock::now() < until, "container tasks were not fully reclaimed");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    } // namespace

    void configure_container(AgentdConfig &config, const std::filesystem::path &settings) {
        require(getpid() == 1, "container agentd must be the PID namespace's init");
        require(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0,
                "cannot protect agentd from task ptrace and descriptor inspection");
        std::ifstream input(settings);
        const auto spec = Json::parse(input);
        require(config.workspace == spec.at("workspace").get<std::string>(),
                "container workspace configuration mismatch");
        config.limits.file_bytes = spec.at("file_bytes");
        config.limits.stdin_bytes = config.limits.file_bytes;
        config.limits.output_bytes = spec.at("output_bytes");
        config.limits.timeout_ms = spec.at("timeout_ms");
        config.task_environment = spec.at("environment").get<std::vector<std::string>>();
        config.finish_tasks = finish_container_tasks;
        config.runtime_info = {
            {"isolation", "oci-crun"}, {"task_uid", geteuid()}, {"network", "disabled"}};
    }
} // namespace agentd
