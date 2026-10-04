#include "workspace/workspace_files.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
    void require(bool value, const char *message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }
    [[noreturn]] void system_error(const char *operation) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }
    struct File {
        int fd;
        explicit File(int value) : fd(value) {
            if (fd < 0) {
                system_error("open workspace file");
            }
        }
        ~File() {
            close(fd);
        }
        File(const File &) = delete;
        File &operator=(const File &) = delete;
    };
    int open_beneath(int root, const fs::path &path, int flags) {
        open_how how{};
        how.flags = flags;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
        return syscall(SYS_openat2, root, path.c_str(), &how, sizeof(how));
    }
    void validate_limit(size_t limit) {
        require(limit > 0 && limit <= 64 * 1024 * 1024, "invalid workspace file limit");
    }
} // namespace

struct WorkspaceFiles::Write::State {
    int parent = -1;
    int fd = -1;
    std::string target;
    std::string temporary;
    size_t limit = 0;
    size_t written = 0;
    mode_t mode = 0644;
    bool committed = false;
    ~State() {
        if (fd >= 0) {
            close(fd);
        }
        if (parent >= 0) {
            if (!committed && !temporary.empty()) {
                unlinkat(parent, temporary.c_str(), 0);
            }
            close(parent);
        }
    }
};

WorkspaceFiles::WorkspaceFiles(const fs::path &root)
    : root_(root.lexically_normal()), root_fd_(-1) {
    require(root.is_absolute() && root.string().find('\0') == std::string::npos,
            "workspace must be an absolute path");
    root_fd_ = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd_ < 0) {
        system_error("open workspace");
    }
}
WorkspaceFiles::~WorkspaceFiles() {
    close(root_fd_);
}

fs::path WorkspaceFiles::relative_file(const fs::path &path) const {
    require(path.is_absolute() && path.string().size() <= 4096 &&
                path.string().find('\0') == std::string::npos,
            "invalid workspace file path");
    for (const auto &part : path) {
        require(part != "..", "path traversal rejected");
    }
    const auto relative = path.lexically_normal().lexically_relative(root_);
    require(!relative.empty() && relative != "." && !relative.is_absolute() &&
                !relative.filename().empty(),
            "expected a file inside workspace");
    for (const auto &part : relative) {
        require(part != "..", "path is outside workspace");
    }
    return relative;
}

std::string WorkspaceFiles::read(const fs::path &path, size_t limit) const {
    validate_limit(limit);
    File file(open_beneath(root_fd_, relative_file(path), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat st{};
    if (fstat(file.fd, &st) != 0) {
        system_error("stat workspace file");
    }
    require(S_ISREG(st.st_mode), "only regular files can be read");
    require(st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= limit,
            "file exceeds read limit");
    std::string content;
    std::array<char, 16384> buffer{};
    for (;;) {
        const auto count = ::read(file.fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            system_error("read workspace file");
        }
        if (count == 0) {
            return content;
        }
        require(static_cast<size_t>(count) <= limit - content.size(), "file exceeds read limit");
        content.append(buffer.data(), static_cast<size_t>(count));
    }
}

std::unique_ptr<WorkspaceFiles::Write> WorkspaceFiles::begin_write(const fs::path &path,
                                                                   size_t limit) const {
    validate_limit(limit);
    const auto relative = relative_file(path);
    auto state = std::make_unique<Write::State>();
    state->limit = limit;
    state->target = relative.filename().string();
    state->parent =
        open_beneath(root_fd_,
                     relative.has_parent_path() ? relative.parent_path() : fs::path("."),
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (state->parent < 0) {
        system_error("open workspace parent");
    }
    const int existing =
        openat(state->parent, state->target.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (existing >= 0) {
        File target(existing);
        struct stat st{};
        if (fstat(target.fd, &st) != 0) {
            system_error("stat write target");
        }
        require(S_ISREG(st.st_mode), "only regular files can be replaced");
        require(st.st_nlink == 1, "hard-linked target rejected");
        state->mode = st.st_mode & 0777;
    } else if (errno != ENOENT) {
        system_error("open write target");
    }
    std::array<unsigned char, 16> random{};
    require(getrandom(random.data(), random.size(), 0) == ssize_t(random.size()),
            "cannot generate temporary filename");
    std::string name = ".sandbox-io-";
    for (auto byte : random) {
        name += "0123456789abcdef"[byte >> 4];
        name += "0123456789abcdef"[byte & 15];
    }
    state->fd = openat(
        state->parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (state->fd < 0) {
        system_error("create temporary workspace file");
    }
    state->temporary = std::move(name);
    return std::unique_ptr<Write>(new Write(std::move(state)));
}
WorkspaceFiles::Write::Write(std::unique_ptr<State> state) : state_(std::move(state)) {
}
WorkspaceFiles::Write::~Write() = default;
void WorkspaceFiles::Write::append(std::string_view bytes) {
    require(!state_->committed, "write transaction already committed");
    require(bytes.size() <= state_->limit - state_->written, "file exceeds write limit");
    while (!bytes.empty()) {
        const auto count = ::write(state_->fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            system_error("write temporary workspace file");
        }
        state_->written += static_cast<size_t>(count);
        bytes.remove_prefix(static_cast<size_t>(count));
    }
}
void WorkspaceFiles::Write::commit() {
    require(!state_->committed, "write transaction already committed");
    if (fchmod(state_->fd, state_->mode) != 0 || fsync(state_->fd) != 0) {
        system_error("finish workspace write");
    }
    if (renameat(
            state_->parent, state_->temporary.c_str(), state_->parent, state_->target.c_str()) !=
        0) {
        system_error("replace workspace file");
    }
    state_->committed = true;
}
