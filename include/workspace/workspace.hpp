#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Host-side API. files_path() contains B, including its task-owned .git.
// The manager's separate baseline repository stays outside the container.

struct Changes {
    std::string status;
    std::string diff;
};

class GitWorkspace {
  public:
    static GitWorkspace create(const std::filesystem::path &repository,
                               const std::filesystem::path &session_directory,
                               const std::string &revision = "HEAD");

    static GitWorkspace open(const std::filesystem::path &session_directory);

    std::filesystem::path files_path() const {
        return session_ / "files";
    }

    const std::filesystem::path &source_repository() const {
        return source_;
    }

    const std::string &baseline() const {
        return baseline_;
    }

    const std::string &source_head() const {
        return source_head_;
    }
    const std::optional<std::string> &source_branch() const {
        return source_branch_;
    }

    std::string status() const;

    // Includes tracked changes and new non-ignored files; stages a private index.
    std::string diff() const;

    // Future: freeze and validate B's commits, then merge or export the result.
    // Define host concurrency rules before adding apply and discard APIs.
  private:
    GitWorkspace(std::filesystem::path session, std::filesystem::path source, std::string baseline);
    void validate_files() const;
    std::filesystem::path session_, source_;
    std::string baseline_, source_head_;
    std::optional<std::string> source_branch_;
};
