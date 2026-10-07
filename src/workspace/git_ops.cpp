#include "workspace/git_ops.hpp"
#include "lib/error.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <git2.h>
#include <git2/sys/config.h>
#include <git2/sys/repository.h>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace {
    constexpr size_t maximum_output = 1024 * 1024;

    void check_git(int code, const char *operation) {
        if (code < 0) {
            const auto *error = git_error_last();
            throw std::runtime_error(std::string(operation) + ": " +
                                     (error ? error->message : "libgit2 operation failed"));
        }
    }

    class Library {
      public:
        Library() {
            check_git(git_libgit2_init(), "initialize libgit2");
        }
        ~Library() {
            git_libgit2_shutdown();
        }
    };

    void initialize_library() {
        static Library library;
    }

    template <class T, void (*Free)(T *)> class Handle {
      public:
        Handle() = default;
        ~Handle() {
            Free(value_);
        }
        Handle(const Handle &) = delete;
        Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {
        }
        auto operator=(const Handle &) -> Handle & = delete;

        auto get() const -> T * {
            return value_;
        }

        auto out() -> T ** {
            return &value_;
        }

      private:
        T *value_ = nullptr;
    };

    using Repository = Handle<git_repository, git_repository_free>;
    using Config = Handle<git_config, git_config_free>;
    using Index = Handle<git_index, git_index_free>;
    using Object = Handle<git_object, git_object_free>;
    using Tree = Handle<git_tree, git_tree_free>;
    using Reference = Handle<git_reference, git_reference_free>;
    using StatusList = Handle<git_status_list, git_status_list_free>;
    using Diff = Handle<git_diff, git_diff_free>;
    using Odb = Handle<git_odb, git_odb_free>;
    using OdbObject = Handle<git_odb_object, git_odb_object_free>;
    using Revwalk = Handle<git_revwalk, git_revwalk_free>;
    using Commit = Handle<git_commit, git_commit_free>;

    auto hex(const git_oid *oid) -> std::string {
        char buffer[GIT_OID_HEXSZ + 1];
        git_oid_tostr(buffer, sizeof(buffer), oid);
        return buffer;
    }

    auto parse_oid(const std::string &text) -> git_oid {
        git_oid oid{};
        check_git(git_oid_fromstr(&oid, text.c_str()), "parse commit ID");
        return oid;
    }

    // Keep configuration per repository. Never change the embedding service's
    // environment, global libgit2 search paths or ownership validation policy.
    void isolate_configuration(git_repository *repository) {
        constexpr char policy[] = "[core]\nfilemode = true\nsymlinks = true\nautocrlf = false\n"
                                  "attributesfile = /dev/null\nexcludesfile = /dev/null\n"
                                  "hookspath = /dev/null\nfsmonitor = false\n";
        Config config;
        check_git(git_config_new(config.out()), "create Git configuration");
        git_config_backend *backend = nullptr;
        check_git(git_config_backend_from_string(&backend, policy, sizeof(policy) - 1, nullptr),
                  "create isolated Git configuration");
        const int added =
            git_config_add_backend(config.get(), backend, GIT_CONFIG_LEVEL_APP, repository, 0);
        if (added < 0) {
            backend->free(backend);
        }
        check_git(added, "attach Git configuration");
        check_git(git_repository_set_config(repository, config.get()), "isolate Git configuration");
    }

    auto open_repository(const fs::path &git_directory, const fs::path &workdir = {})
        -> Repository {
        Repository repository;
        // These paths are explicitly selected trusted A or private manager data.
        // Bare opening also supports sudo access without changing global safe.directory.
        check_git(git_repository_open_bare(repository.out(), git_directory.c_str()),
                  "open Git object store");
        isolate_configuration(repository.get());
        if (!workdir.empty()) {
            check_git(git_repository_set_workdir(repository.get(), workdir.c_str(), 0),
                      "associate Git working files");
        }
        return repository;
    }

    auto initialize_repository(const fs::path &path, bool bare) -> Repository {
        Repository repository;
        git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
        options.flags = GIT_REPOSITORY_INIT_NO_REINIT | GIT_REPOSITORY_INIT_MKPATH;
        if (bare) {
            options.flags |= GIT_REPOSITORY_INIT_BARE;
        }
        check_git(git_repository_init_ext(repository.out(), path.c_str(), &options),
                  "initialize Git repository");
        isolate_configuration(repository.get());
        return repository;
    }

    auto lookup_tree(git_repository *repository, const git_oid *oid) -> Tree {
        Tree tree;
        check_git(git_tree_lookup(tree.out(), repository, oid), "read Git tree");
        return tree;
    }

    void reject_submodules(git_repository *repository, const git_tree *tree) {
        for (size_t i = 0; i < git_tree_entrycount(tree); ++i) {
            const auto *entry = git_tree_entry_byindex(tree, i);
            lib::require(git_tree_entry_filemode(entry) != GIT_FILEMODE_COMMIT,
                         "submodules are not supported");
            if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
                const auto child = lookup_tree(repository, git_tree_entry_id(entry));
                reject_submodules(repository, child.get());
            }
        }
    }

    class ObjectImporter {
      public:
        ObjectImporter(git_repository *source, git_repository *destination)
            : source_(source),
              deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(60)) {
            check_git(git_repository_odb(source_odb_.out(), source), "open source objects");
            check_git(git_repository_odb(destination_odb_.out(), destination),
                      "open destination objects");
        }

        void history(const git_oid &baseline) {
            Revwalk walk;
            check_git(git_revwalk_new(walk.out(), source_), "create history traversal");
            check_git(git_revwalk_push(walk.get(), &baseline), "select baseline history");
            git_oid oid;
            int next;
            while ((next = git_revwalk_next(&oid, walk.get())) == 0) {
                copy_object(&oid);
                Commit commit;
                check_git(git_commit_lookup(commit.out(), source_, &oid), "read commit");
                copy_tree(git_commit_tree_id(commit.get()));
            }
            if (next != GIT_ITEROVER) {
                check_git(next, "traverse baseline history");
            }
        }

      private:
        void copy_object(const git_oid *oid) {
            lib::require(std::chrono::steady_clock::now() < deadline_,
                         "Git object import timed out");
            if (!seen_.insert(hex(oid)).second) {
                return;
            }
            OdbObject object;
            check_git(git_odb_read(object.out(), source_odb_.get(), oid), "read Git object");
            git_oid written;
            check_git(
                git_odb_write(&written, destination_odb_.get(), git_odb_object_data(object.get()),
                              git_odb_object_size(object.get()), git_odb_object_type(object.get())),
                "copy Git object");
            lib::require(git_oid_equal(oid, &written), "copied Git object has an unexpected ID");
        }

        void copy_tree(const git_oid *oid) {
            if (seen_.count(hex(oid))) {
                return;
            }
            copy_object(oid);
            const auto tree = lookup_tree(source_, oid);
            for (size_t i = 0; i < git_tree_entrycount(tree.get()); ++i) {
                const auto *entry = git_tree_entry_byindex(tree.get(), i);
                if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
                    copy_tree(git_tree_entry_id(entry));
                } else if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
                    copy_object(git_tree_entry_id(entry));
                }
            }
        }

        git_repository *source_;
        Odb source_odb_, destination_odb_;
        std::unordered_set<std::string> seen_;
        std::chrono::steady_clock::time_point deadline_;
    };

    auto read_status(git_repository *repository) -> StatusList {
        git_status_options options = GIT_STATUS_OPTIONS_INIT;
        options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
        StatusList status;
        check_git(git_status_list_new(status.out(), repository, &options), "inspect Git files");
        return status;
    }

    auto status_code(unsigned int flags, bool index) -> char {
        if (flags & GIT_STATUS_CONFLICTED) {
            return 'U';
        }
        const unsigned int added = index ? GIT_STATUS_INDEX_NEW : GIT_STATUS_WT_NEW;
        const unsigned int deleted = index ? GIT_STATUS_INDEX_DELETED : GIT_STATUS_WT_DELETED;
        const unsigned int changed = index ? GIT_STATUS_INDEX_MODIFIED : GIT_STATUS_WT_MODIFIED;
        const unsigned int type = index ? GIT_STATUS_INDEX_TYPECHANGE : GIT_STATUS_WT_TYPECHANGE;
        if (flags & added) {
            return index ? 'A' : '?';
        }
        if (flags & deleted) {
            return 'D';
        }
        if (flags & type) {
            return 'T';
        }
        return flags & changed ? 'M' : ' ';
    }

    auto quoted_path(const char *path) -> std::string {
        std::string escaped;
        bool quote = false;
        for (const unsigned char ch : std::string(path)) {
            if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '"' || ch == '\\') {
                quote = true;
                escaped += '\\';
                escaped += ch == '\n' ? 'n' : ch == '\r' ? 'r' : ch == '\t' ? 't' : char(ch);
            } else if (ch < 32 || ch >= 127) {
                quote = true;
                escaped += '\\';
                escaped += static_cast<char>('0' + (ch >> 6));
                escaped += static_cast<char>('0' + ((ch >> 3) & 7));
                escaped += static_cast<char>('0' + (ch & 7));
            } else {
                quote = quote || ch == ' ';
                escaped += static_cast<char>(ch);
            }
        }
        return quote ? '"' + escaped + '"' : escaped;
    }

    struct PatchOutput {
        std::string text;
        std::exception_ptr error;
        bool limited = false;
    };

    auto append_patch(const git_diff_delta *, const git_diff_hunk *, const git_diff_line *line,
                      void *payload) noexcept -> int {
        auto &output = *static_cast<PatchOutput *>(payload);
        try {
            const bool prefix = line->origin == '+' || line->origin == '-' || line->origin == ' ';
            if (line->content_len + size_t(prefix) > maximum_output - output.text.size()) {
                output.limited = true;
                return GIT_EUSER;
            }
            if (prefix) {
                output.text += line->origin;
            }
            output.text.append(line->content, line->content_len);
            return 0;
        } catch (...) {
            output.error = std::current_exception();
            return GIT_EUSER;
        }
    }
} // namespace

namespace git_ops {

    auto inspect_source(const fs::path &requested, const std::string &revision) -> SourceSnapshot {
        initialize_library();
        auto root = fs::canonical(requested);
        lib::require(fs::is_directory(root), "repository must be a directory");
        while (!fs::exists(root / ".git")) {
            lib::require(root != root.root_path(), "no Git worktree found");
            root = root.parent_path();
        }

        git_buf discovered = GIT_BUF_INIT;
        const int discovery = git_repository_discover(&discovered, root.c_str(), 0, nullptr);
        const std::string gitdir = discovered.ptr ? discovered.ptr : "";
        git_buf_dispose(&discovered);
        check_git(discovery, "discover source Git repository");

        auto repository = open_repository(gitdir, root);
        const auto status = read_status(repository.get());
        lib::require(git_status_list_entrycount(status.get()) == 0,
                     "source repository is dirty; this version requires committed input");

        Object baseline;
        check_git(
            git_revparse_single(baseline.out(), repository.get(), (revision + "^{commit}").c_str()),
            "resolve baseline commit");

        Commit commit;
        check_git(git_commit_lookup(commit.out(), repository.get(), git_object_id(baseline.get())),
                  "read baseline commit");

        const auto tree = lookup_tree(repository.get(), git_commit_tree_id(commit.get()));
        reject_submodules(repository.get(), tree.get());
        Reference head;
        check_git(git_repository_head(head.out(), repository.get()), "read source HEAD");

        SourceSnapshot result{root, hex(git_object_id(baseline.get())),
                              hex(git_reference_target(head.get())), std::nullopt};

        if (git_reference_is_branch(head.get())) {
            result.branch = git_reference_shorthand(head.get());
        }
        return result;
    }

    void initialize_snapshot(const SourceSnapshot &source, const fs::path &workspace_directory) {
        initialize_library();
        git_buf discovered = GIT_BUF_INIT;
        const int code =
            git_repository_discover(&discovered, source.repository.c_str(), 0, nullptr);
        const std::string gitdir = discovered.ptr ? discovered.ptr : "";
        git_buf_dispose(&discovered);
        check_git(code, "discover source object store");
        const auto original = open_repository(gitdir);

        auto manager = initialize_repository(workspace_directory / "manager.git", true);
        auto task = initialize_repository(workspace_directory / "files", false);
        const auto baseline = parse_oid(source.baseline);
        ObjectImporter(original.get(), manager.get()).history(baseline);
        ObjectImporter(manager.get(), task.get()).history(baseline);
        Reference manager_head, task_branch;
        check_git(
            git_reference_create(manager_head.out(), manager.get(), "HEAD", &baseline, 1, nullptr),
            "record private baseline");
        check_git(git_reference_create(task_branch.out(), task.get(), "refs/heads/sandbox",
                                       &baseline, 0, nullptr),
                  "create task branch");
        check_git(git_repository_set_head(task.get(), "refs/heads/sandbox"), "select task branch");
        git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
        checkout.checkout_strategy = GIT_CHECKOUT_FORCE;
        check_git(git_checkout_head(task.get(), &checkout), "check out task files");
        Commit commit;
        check_git(git_commit_lookup(commit.out(), manager.get(), &baseline),
                  "load private baseline");
        const auto tree = lookup_tree(manager.get(), git_commit_tree_id(commit.get()));
        Index index;
        check_git(git_repository_index(index.out(), manager.get()), "open private index");
        check_git(git_index_read_tree(index.get(), tree.get()), "initialize private index");
        check_git(git_index_write(index.get()), "save private index");
    }

    auto status(const fs::path &workspace_directory) -> std::string {
        initialize_library();
        const auto repository =
            open_repository(workspace_directory / "manager.git", workspace_directory / "files");
        const auto status = read_status(repository.get());
        std::string output;
        for (size_t i = 0; i < git_status_list_entrycount(status.get()); ++i) {
            const auto *entry = git_status_byindex(status.get(), i);
            if (entry->status == GIT_STATUS_CURRENT) {
                continue;
            }
            const auto *delta =
                entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
            if (entry->status == GIT_STATUS_WT_NEW) {
                output += "??";
            } else {
                output += status_code(entry->status, true);
                output += status_code(entry->status, false);
            }
            output += ' ';
            output +=
                quoted_path(delta->old_file.path ? delta->old_file.path : delta->new_file.path);
            output += '\n';
            lib::require(output.size() <= maximum_output, "Git status exceeded 1 MiB");
        }
        return output;
    }

    auto diff(const fs::path &workspace_directory, const std::string &baseline) -> std::string {
        initialize_library();
        const auto repository =
            open_repository(workspace_directory / "manager.git", workspace_directory / "files");
        Index index;
        check_git(git_repository_index(index.out(), repository.get()), "open private index");
        check_git(git_index_update_all(index.get(), nullptr, nullptr, nullptr),
                  "refresh tracked files");
        check_git(git_index_add_all(index.get(), nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr),
                  "stage private snapshot");
        check_git(git_index_write(index.get()), "save private snapshot");
        const auto oid = parse_oid(baseline);
        Commit commit;
        check_git(git_commit_lookup(commit.out(), repository.get(), &oid), "read baseline");
        const auto tree = lookup_tree(repository.get(), git_commit_tree_id(commit.get()));
        git_diff_options options = GIT_DIFF_OPTIONS_INIT;
        options.flags = GIT_DIFF_SHOW_BINARY;
        Diff difference;
        check_git(git_diff_tree_to_index(difference.out(), repository.get(), tree.get(),
                                         index.get(), &options),
                  "compare private snapshot");
        PatchOutput output;
        const int printed =
            git_diff_print(difference.get(), GIT_DIFF_FORMAT_PATCH, append_patch, &output);
        if (output.error) {
            std::rethrow_exception(output.error);
        }
        lib::require(!output.limited, "Git diff exceeded 1 MiB; operation rejected");
        check_git(printed, "generate Git patch");
        return output.text;
    }
} // namespace git_ops
