#include "workspace/workspace_backend.hpp"

namespace {
    class GitWorkspaceBackend final : public WorkspaceBackend {
      public:
        std::string id() const override {
            return "git";
        }

        WorkspaceSnapshot create(const std::filesystem::path &source,
                                 const std::filesystem::path &directory,
                                 const std::string &revision) override {
            const auto workspace = GitWorkspace::create(source, directory, revision);
            return {workspace.source_repository(),
                    workspace.files_path(),
                    workspace.baseline(),
                    workspace.source_head(),
                    workspace.source_branch()};
        }

        Changes inspect(const std::filesystem::path &directory) override {
            const auto workspace = GitWorkspace::open(directory);
            return {workspace.status(), workspace.diff()};
        }
    };
} // namespace

std::unique_ptr<WorkspaceBackend> make_git_workspace_backend() {
    return std::make_unique<GitWorkspaceBackend>();
}
