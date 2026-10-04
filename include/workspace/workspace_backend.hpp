#pragma once

#include "workspace.hpp"
#include <memory>
#include <optional>

struct WorkspaceSnapshot {
    std::filesystem::path source_repository;
    std::filesystem::path files_directory;
    std::string baseline;
    std::string source_head;
    std::optional<std::string> source_branch;
};

class WorkspaceBackend {
  public:
    virtual ~WorkspaceBackend() = default;
    virtual std::string id() const = 0;
    virtual WorkspaceSnapshot create(const std::filesystem::path &source,
                                     const std::filesystem::path &directory,
                                     const std::string &revision) = 0;
    // The caller must quiesce writers and synchronize guest files first.
    virtual Changes inspect(const std::filesystem::path &directory) = 0;
};

std::unique_ptr<WorkspaceBackend> make_git_workspace_backend();
