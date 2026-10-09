#include "agentd/workspace_files.hpp"
#include "agentd/task_runner.hpp"
#include "lib/error.hpp"
#include "lib/filesystem.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace agentd {
    namespace {
        class FileIdentity {
          public:
            explicit FileIdentity(bool mapped) : active_(mapped) {
                if (active_) {
                    previous_gid_ = setfsgid(task_gid);
                    previous_uid_ = setfsuid(task_uid);
                    if (static_cast<uid_t>(setfsuid(-1)) != task_uid ||
                        static_cast<gid_t>(setfsgid(-1)) != task_gid) {
                        setfsuid(previous_uid_);
                        setfsgid(previous_gid_);
                        throw std::runtime_error("cannot enter agentd file identity");
                    }
                }
            }
            ~FileIdentity() {
                if (active_) {
                    setfsuid(previous_uid_);
                    setfsgid(previous_gid_);
                }
            }

          private:
            bool active_;
            int previous_uid_ = 0;
            int previous_gid_ = 0;
        };

        void validate_limit(size_t limit) {
            lib::require(limit > 0 && limit <= 64 * 1024 * 1024, "invalid workspace file limit");
        }
    } // namespace

    WorkspaceFiles::WorkspaceFiles(const fs::path &root, bool mapped_identity)
        : root_(root.lexically_normal()), mapped_identity_(mapped_identity) {
        lib::require(root.is_absolute() && root.string().find('\0') == std::string::npos,
                     "workspace must be an absolute path");
        root_fd_ = lib::UniqueFd(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root_fd_.get() < 0) {
            lib::system_error("open workspace");
        }
    }

    auto WorkspaceFiles::relative_file(const fs::path &path) const -> fs::path {
        lib::require(path.is_absolute() && path.string().size() <= 4096 &&
                         path.string().find('\0') == std::string::npos,
                     "invalid workspace file path");
        for (const auto &part : path) {
            lib::require(part != "..", "path traversal rejected");
        }
        const auto relative = path.lexically_normal().lexically_relative(root_);
        lib::require(!relative.empty() && relative != "." && !relative.is_absolute() &&
                         !relative.filename().empty(),
                     "expected a file inside workspace");
        for (const auto &part : relative) {
            lib::require(part != "..", "path is outside workspace");
        }
        return relative;
    }

    auto WorkspaceFiles::read(const fs::path &path, size_t limit) const -> std::string {
        FileIdentity identity(mapped_identity_);
        validate_limit(limit);

        // 这里使用了openat2保证读取安全
        lib::UniqueFd file(
            lib::open_beneath(root_fd_.get(), relative_file(path), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        if (file.get() < 0) {
            lib::system_error("open workspace file");
        }
        struct stat st{};
        if (fstat(file.get(), &st) != 0) {
            lib::system_error("stat workspace file");
        }
        lib::require(S_ISREG(st.st_mode), "only regular files can be read");
        lib::require(st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= limit,
                     "file exceeds read limit");
        std::string content;
        std::array<char, 16384> buffer{};
        for (;;) {
            const auto count = ::read(file.get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                lib::system_error("read workspace file");
            }
            if (count == 0) {
                return content;
            }
            lib::require(static_cast<size_t>(count) <= limit - content.size(),
                         "file exceeds read limit");
            content.append(buffer.data(), static_cast<size_t>(count));
        }
    }

    auto WorkspaceFiles::begin_write(const fs::path &path, size_t limit) const
        -> std::unique_ptr<WorkspaceFiles::Write> {
        FileIdentity identity(mapped_identity_);
        validate_limit(limit);
        const auto relative = relative_file(path);
        auto transaction = std::unique_ptr<Write>(new Write(mapped_identity_));
        transaction->limit_ = limit;
        transaction->target_ = relative.filename().string();
        transaction->parent_ = lib::UniqueFd(lib::open_beneath(
            root_fd_.get(), relative.has_parent_path() ? relative.parent_path() : fs::path("."),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (transaction->parent_.get() < 0) {
            lib::system_error("open workspace parent");
        }
        const int existing = openat(transaction->parent_.get(), transaction->target_.c_str(),
                                    O_PATH | O_NOFOLLOW | O_CLOEXEC);

        if (existing >= 0) {
            lib::UniqueFd target(existing);
            struct stat st{};
            if (fstat(target.get(), &st) != 0) {
                lib::system_error("stat write target");
            }
            lib::require(S_ISREG(st.st_mode), "only regular files can be replaced");
            lib::require(st.st_nlink == 1, "hard-linked target rejected");
            transaction->mode_ = st.st_mode & 0777;
        } else if (errno != ENOENT) {
            lib::system_error("open write target");
        }

        std::array<unsigned char, 16> random{};
        lib::require(getrandom(random.data(), random.size(), 0) == ssize_t(random.size()),
                     "cannot generate temporary filename");
        std::string name = ".sandbox-io-";
        for (auto byte : random) {
            name += "0123456789abcdef"[byte >> 4];
            name += "0123456789abcdef"[byte & 15];
        }
        transaction->file_ = lib::UniqueFd(
            openat(transaction->parent_.get(), name.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (transaction->file_.get() < 0) {
            lib::system_error("create temporary workspace file");
        }
        transaction->temporary_ = std::move(name);
        return transaction;
    }

    WorkspaceFiles::Write::Write(bool mapped_identity) : mapped_identity_(mapped_identity) {
    }

    WorkspaceFiles::Write::~Write() {
        if (!committed_ && !temporary_.empty()) {
            try {
                FileIdentity identity(mapped_identity_);
                unlinkat(parent_.get(), temporary_.c_str(), 0);
            } catch (...) {
                // Cleanup cannot throw during connection teardown.
            }
        }
    }

    void WorkspaceFiles::Write::append(std::string_view bytes) {
        lib::require(!committed_, "write transaction already committed");
        lib::require(bytes.size() <= limit_ - written_, "file exceeds write limit");
        while (!bytes.empty()) {
            const auto count = ::write(file_.get(), bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                lib::system_error("write temporary workspace file");
            }
            written_ += static_cast<size_t>(count);
            bytes.remove_prefix(static_cast<size_t>(count));
        }
    }

    void WorkspaceFiles::Write::commit() {
        FileIdentity identity(mapped_identity_);
        lib::require(!committed_, "write transaction already committed");
        if (fchmod(file_.get(), mode_) != 0 || fsync(file_.get()) != 0) {
            lib::system_error("finish workspace write");
        }
        if (renameat(parent_.get(), temporary_.c_str(), parent_.get(), target_.c_str()) != 0) {
            lib::system_error("replace workspace file");
        }
        committed_ = true;
    }
} // namespace agentd
