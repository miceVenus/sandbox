#include "lib.hpp"
#include "resources.hpp"
#include "workspace/git/git_repository_ops.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {
    std::string invoke(const std::vector<std::string> &arguments) {
        const auto worker = sandbox_resources::git_worker_path();
        require(std::filesystem::is_regular_file(worker) && access(worker.c_str(), X_OK) == 0,
                "SDK Git worker missing or not executable: " + worker.string());
        std::vector<std::string> command{worker.string()};
        command.insert(command.end(), arguments.begin(), arguments.end());
        const auto result = run_process(command, 60000, 1024 * 1024);
        require(!result.timed_out, "Git operation timed out");
        require(!result.output_limited, "Git output exceeded 1 MiB; operation rejected");
        require(result.runtime_status == 0, "Git operation failed: " + result.err);
        return result.out;
    }
} // namespace

namespace git_storage {
    SourceSnapshot inspect_source(const std::filesystem::path &requested,
                                  const std::string &revision) {
        const auto record =
            nlohmann::json::parse(invoke({"inspect-source", requested.string(), revision}));
        SourceSnapshot snapshot;
        snapshot.repository = record.at("repository").get<std::string>();
        snapshot.baseline = record.at("baseline");
        snapshot.head = record.at("head");
        if (!record.at("branch").is_null()) {
            snapshot.branch = record.at("branch").get<std::string>();
        }
        return snapshot;
    }

    void initialize_snapshot(const SourceSnapshot &source, const std::filesystem::path &session) {
        invoke({"initialize", source.repository.string(), source.baseline, session.string()});
    }

    std::string status(const std::filesystem::path &session) {
        return invoke({"status", session.string()});
    }

    std::string diff(const std::filesystem::path &session, const std::string &baseline) {
        return invoke({"diff", session.string(), baseline});
    }
} // namespace git_storage
