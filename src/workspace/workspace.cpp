#include "workspace/workspace.hpp"
#include "lib/filesystem.hpp"
#include "lib/string.hpp"
#include "workspace/git_ops.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <sys/stat.h>

namespace fs = std::filesystem;

Workspace::Workspace(fs::path workspace_directory, fs::path source, std::string baseline)
    : directory_(std::move(workspace_directory)), source_(std::move(source)),
      baseline_(std::move(baseline)) {
}

auto Workspace::create(const fs::path &repository, const fs::path &sandbox_directory,
                       const std::string &revision) -> Workspace {
    if (revision.empty() || revision.front() == '-') {
        throw std::runtime_error("invalid revision");
    }
    const auto snapshot = git_ops::inspect_source(repository, revision);
    const auto &source = snapshot.repository;
    const auto &baseline = snapshot.baseline;

    // Require an existing parent; canonicalize it but do not follow the leaf.
    const fs::path requested = fs::absolute(sandbox_directory).lexically_normal();
    const fs::path workspace_directory =
        fs::canonical(requested.parent_path()) / requested.filename();

    if (workspace_directory == source ||
        workspace_directory.string().rfind(source.string() + "/", 0) == 0) {
        throw std::runtime_error("workspace directory must be outside the source repository");
    }

    if (!fs::create_directory(workspace_directory)) {
        throw std::runtime_error("workspace directory already exists");
    }

    Workspace workspace(workspace_directory, source, baseline);

    try {
        fs::permissions(workspace_directory, fs::perms::owner_all);
        fs::create_directory(workspace.files_path());

        git_ops::initialize_snapshot(snapshot, workspace_directory);
        workspace.source_head_ = snapshot.head;
        workspace.source_branch_ = snapshot.branch;
        workspace.validate_files();
        std::ofstream state(workspace_directory / "workspace.txt");
        state << "sandbox-workspace-v1\n"
              << std::quoted(source.string()) << '\n'
              << baseline << '\n';
        state.close();
        if (!state) {
            throw std::runtime_error("failed to write workspace metadata");
        }
    } catch (...) {
        // This directory was exclusively created above; never remove the source.
        fs::remove_all(workspace_directory);
        throw;
    }
    return workspace;
}

auto Workspace::open(const fs::path &requested_directory) -> Workspace {
    lib::require_directory(requested_directory);
    const fs::path workspace_directory = fs::canonical(requested_directory);
    lib::require_directory(workspace_directory / "manager.git");
    lib::require_directory(workspace_directory / "files");
    lib::require_directory(workspace_directory / "files/.git");
    // Read legacy workspace records, but always create the new workspace.txt format.
    const auto metadata = fs::exists(workspace_directory / "workspace.txt")
                              ? workspace_directory / "workspace.txt"
                              : workspace_directory / "session.txt";
    if (fs::symlink_status(metadata).type() != fs::file_type::regular) {
        throw std::runtime_error("invalid workspace metadata");
    }

    std::ifstream state(metadata);
    std::string marker, source, baseline;
    std::getline(state, marker);
    state >> std::quoted(source) >> baseline;
    if (!state || marker != "sandbox-workspace-v1" ||
        !((baseline.size() == 40 || baseline.size() == 64) && lib::is_hex(baseline))) {
        throw std::runtime_error("invalid workspace metadata");
    }
    return {workspace_directory, source, baseline};
}

void Workspace::validate_files() const {

    lib::require_directory(files_path());
    lib::require_directory(files_path() / ".git");

    // B's top-level .git is the task repository. Never traverse or trust it
    // for manager status/diff; reject other embedded Git metadata and special files.
    for (auto it = fs::recursive_directory_iterator(files_path());
         it != fs::recursive_directory_iterator(); ++it) {
        const auto &entry = *it;
        const auto type = entry.symlink_status().type();
        auto name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) -> char {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        });
        if (name == ".git") {
            if (entry.path() == files_path() / ".git" && type == fs::file_type::directory) {
                it.disable_recursion_pending();
                continue;
            }
            throw std::runtime_error("unexpected .git path in workspace");
        }
        if (type != fs::file_type::regular && type != fs::file_type::directory &&
            type != fs::file_type::symlink) {
            throw std::runtime_error("special workspace files are not supported");
        }
        if (type == fs::file_type::regular) {
            struct stat info{};
            if (lstat(entry.path().c_str(), &info) != 0 || info.st_nlink > 1) {
                throw std::runtime_error("hard-linked files are not supported");
            }
        }
    }
}

auto Workspace::status() const -> std::string {
    validate_files();
    return git_ops::status(directory_);
}

auto Workspace::diff() const -> std::string {
    validate_files();
    return git_ops::diff(directory_, baseline_);
}
