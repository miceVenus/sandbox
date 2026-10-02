#include "workspace.hpp"
#include "process.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace fs = std::filesystem;
namespace {
    // Discover the selected worktree without first asking Git to read its config.
    // safe.directory must name the worktree root, not a requested subdirectory.
    fs::path worktree_root(const fs::path &requested) {
        fs::path path = fs::canonical(requested);
        if (!fs::is_directory(path)) {
            throw std::runtime_error("repository must be a directory");
        }
        for (;;) {
            if (fs::exists(path / ".git")) {
                return path; // Directory or linked-worktree .git file.
            }
            if (path == path.root_path()) {
                break;
            }
            path = path.parent_path();
        }
        throw std::runtime_error("no Git worktree found for: " + requested.string());
    }

    std::string trim_newline(std::string text) {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        return text;
    }

    // No global/system Git configuration, hooks, external diff or fsmonitor.
    // The source repository remains administrator-selected and trusted.
    std::string execute_git(const std::vector<std::string> &options) {
        std::vector<std::string> args{"/usr/bin/env",
                                      "GIT_CONFIG_NOSYSTEM=1",
                                      "GIT_CONFIG_GLOBAL=/dev/null",
                                      "GIT_TERMINAL_PROMPT=0",
                                      "/usr/bin/git",
                                      "-c",
                                      "core.hooksPath=/dev/null",
                                      "-c",
                                      "core.fsmonitor=false",
                                      "-c",
                                      "core.attributesFile=/dev/null",
                                      "-c",
                                      "core.excludesFile=/dev/null"};
        if (options.size() >= 2 && options[0] == "-C") {
            args.push_back("-c");
            args.push_back("safe.directory=" + worktree_root(options[1]).string());
        }
        args.insert(args.end(), options.begin(), options.end());
        const Result result = run_process(args, 60000);
        if (result.timed_out) {
            throw std::runtime_error("Git operation timed out");
        }
        if (result.output_limited) {
            throw std::runtime_error("Git output exceeded 1 MiB; operation rejected");
        }
        if (result.runtime_status != 0) {
            throw std::runtime_error("Git failed: " + result.err);
        }
        return result.out;
    }

    bool valid_hash(const std::string &value) {
        return (value.size() == 40 || value.size() == 64) &&
               std::all_of(value.begin(), value.end(), [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }

    void require_directory(const fs::path &path) {
        if (fs::symlink_status(path).type() != fs::file_type::directory) {
            throw std::runtime_error("expected a real directory: " + path.string());
        }
    }
} // namespace

GitWorkspace::GitWorkspace(fs::path session, fs::path source, std::string baseline)
    : session_(std::move(session)), source_(std::move(source)), baseline_(std::move(baseline)) {
}

std::string GitWorkspace::git(const std::vector<std::string> &arguments) const {
    std::vector<std::string> args{"-c",
                                  "safe.directory=" + source_.string(),
                                  "--git-dir=" + (session_ / "manager.git").string(),
                                  "--work-tree=" + files_path().string()};
    args.insert(args.end(), arguments.begin(), arguments.end());
    return execute_git(args);
}

GitWorkspace GitWorkspace::create(const fs::path &repository,
                                  const fs::path &session_directory,
                                  const std::string &revision) {
    if (revision.empty() || revision.front() == '-') {
        throw std::runtime_error("invalid revision");
    }
    const fs::path input = fs::canonical(repository);
    const fs::path source = fs::canonical(
        trim_newline(execute_git({"-C", input.string(), "rev-parse", "--show-toplevel"})));
    if (!execute_git({"-C", source.string(), "status", "--porcelain=v1", "--untracked-files=all"})
             .empty()) {
        throw std::runtime_error(
            "source repository is dirty; this version requires committed input");
    }
    const std::string baseline = trim_newline(execute_git({"-C",
                                                           source.string(),
                                                           "rev-parse",
                                                           "--verify",
                                                           "--end-of-options",
                                                           revision + "^{commit}"}));
    if (!valid_hash(baseline)) {
        throw std::runtime_error("invalid baseline hash");
    }
    const auto tree = execute_git({"-C", source.string(), "ls-tree", "-r", baseline});
    if (tree.rfind("160000 ", 0) == 0 || tree.find("\n160000 ") != std::string::npos) {
        throw std::runtime_error("submodules are not supported in the first version");
    }

    // Require an existing parent; canonicalize it but do not follow the leaf.
    const fs::path requested = fs::absolute(session_directory).lexically_normal();
    const fs::path session = fs::canonical(requested.parent_path()) / requested.filename();
    if (session == source || session.string().rfind(source.string() + "/", 0) == 0) {
        throw std::runtime_error("session directory must be outside the source repository");
    }
    if (!fs::create_directory(session)) {
        throw std::runtime_error("session directory already exists");
    }
    GitWorkspace workspace(session, source, baseline);
    try {
        fs::permissions(session, fs::perms::owner_all);
        fs::create_directory(workspace.files_path());
        execute_git({"init", "--bare", (session / "manager.git").string()});
        // Keep a manager-owned baseline independent of the task's writable Git repo.
        workspace.git({"fetch", "--no-tags", source.string(), baseline});
        workspace.git({"update-ref", "HEAD", baseline});
        workspace.git({"read-tree", baseline});

        // B is a real repository. Agent commands may commit on its own branch;
        // the manager never reads B's config, hooks or refs for status/diff.
        execute_git({"init", "--quiet", workspace.files_path().string()});
        execute_git({"-C",
                     workspace.files_path().string(),
                     "fetch",
                     "--no-tags",
                     "--no-write-fetch-head",
                     (session / "manager.git").string(),
                     baseline + ":refs/heads/agent"});
        execute_git({"-C",
                     workspace.files_path().string(),
                     "checkout",
                     "--quiet",
                     "--force",
                     "agent"});
        workspace.validate_files();
        std::ofstream state(session / "session.txt");
        state << "sandbox-workspace-v1\n"
              << std::quoted(source.string()) << '\n'
              << baseline << '\n';
        state.close();
        if (!state) {
            throw std::runtime_error("failed to write session metadata");
        }
    } catch (...) {
        // This directory was exclusively created above; never remove the source.
        fs::remove_all(session);
        throw;
    }
    return workspace;
}

GitWorkspace GitWorkspace::open(const fs::path &session_directory) {
    require_directory(session_directory);
    const fs::path session = fs::canonical(session_directory);
    require_directory(session / "manager.git");
    require_directory(session / "files");
    require_directory(session / "files/.git");
    if (fs::symlink_status(session / "session.txt").type() != fs::file_type::regular) {
        throw std::runtime_error("invalid session metadata");
    }
    std::ifstream state(session / "session.txt");
    std::string marker, source, baseline;
    std::getline(state, marker);
    state >> std::quoted(source) >> baseline;
    if (!state || marker != "sandbox-workspace-v1" || !valid_hash(baseline)) {
        throw std::runtime_error("invalid session metadata");
    }
    return GitWorkspace(session, source, baseline);
}

void GitWorkspace::validate_files() const {
    require_directory(files_path());
    require_directory(files_path() / ".git");
    // B's top-level .git is the agent repository. Never traverse or trust it
    // for manager status/diff; reject other embedded Git metadata and special files.
    for (auto it = fs::recursive_directory_iterator(files_path());
         it != fs::recursive_directory_iterator();
         ++it) {
        const auto &entry = *it;
        const auto type = entry.symlink_status().type();
        auto name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
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

std::string GitWorkspace::status() const {
    validate_files();
    return git({"status", "--porcelain=v1", "--untracked-files=all"});
}

std::string GitWorkspace::diff() const {
    validate_files();
    // Staging here is private metadata only: the source index remains untouched.
    git({"add", "--all", "--", "."});
    return git({"diff", "--cached", "--binary", "--no-ext-diff", "--no-textconv", baseline_, "--"});
}

int workspace_cli(int argc, char **argv) {
    if (argc < 1) {
        throw std::runtime_error(
            "workspace create REPO SESSION_DIR [REV] | status SESSION_DIR | diff SESSION_DIR");
    }
    const std::string action = argv[0];
    if (action == "create" && (argc == 3 || argc == 4)) {
        const auto workspace = GitWorkspace::create(argv[1], argv[2], argc == 4 ? argv[3] : "HEAD");
        std::cout << "baseline: " << workspace.baseline() << '\n'
                  << "files: " << workspace.files_path().string() << '\n';
    } else if ((action == "status" || action == "diff") && argc == 2) {
        const auto workspace = GitWorkspace::open(argv[1]);
        std::cout << (action == "status" ? workspace.status() : workspace.diff());
    } else {
        throw std::runtime_error("invalid workspace arguments");
    }
    return 0;
}
