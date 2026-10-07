#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Host-side API. files_path() contains B, including its task-owned .git.
// The manager's separate baseline repository stays outside the container.

struct Changes {
    std::string status;
    std::string diff;
};

class Workspace {
  public:
    static auto create(const std::filesystem::path &repository,
                       const std::filesystem::path &sandbox_directory,
                       const std::string &revision = "HEAD") -> Workspace;

    static auto open(const std::filesystem::path &workspace_directory) -> Workspace;

    [[nodiscard]] auto files_path() const -> std::filesystem::path {
        return directory_ / "files";
    }

    [[nodiscard]] auto source_repository() const -> const std::filesystem::path & {
        return source_;
    }

    [[nodiscard]] auto baseline() const -> const std::string & {
        return baseline_;
    }

    [[nodiscard]] auto source_head() const -> const std::string & {
        return source_head_;
    }
    [[nodiscard]] auto source_branch() const -> const std::optional<std::string> & {
        return source_branch_;
    }

    [[nodiscard]] auto status() const -> std::string;

    // Includes tracked changes and new non-ignored files; stages a private index.
    [[nodiscard]] auto diff() const -> std::string;

    // Future: freeze and validate B's commits, then merge or export the result.
    // Define host concurrency rules before adding apply and discard APIs.
  private:
    Workspace(std::filesystem::path workspace_directory, std::filesystem::path source,
              std::string baseline);
    void validate_files() const;
    std::filesystem::path directory_, source_;
    std::string baseline_, source_head_;
    std::optional<std::string> source_branch_;
};
