// Runs inside the container. No host workspace paths are accepted here.
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <linux/openat2.h>
#include <stdexcept>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    void require(bool ok, const std::string &message) {
        if (!ok) {
            throw std::runtime_error(message);
        }
    }

    void system_error(const std::string &operation) {
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }

    struct File {
        int fd;
        explicit File(int value) : fd(value) {
            if (fd < 0) {
                system_error("open file");
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
        // Fail closed on kernels without openat2; do not fall back to path checks.
        return syscall(SYS_openat2, root, path.c_str(), &how, sizeof(how));
    }

    fs::path relative_file(const fs::path &root, const fs::path &path) {
        require(root.is_absolute() && path.is_absolute(), "expected absolute container paths");
        require(path.string().size() <= 4096, "path too long");
        for (const auto &part : path) {
            require(part != "..", "path traversal rejected");
        }
        const auto relative = path.lexically_normal().lexically_relative(root);
        require(!relative.empty() && relative != "." && !relative.is_absolute(),
                "expected a file inside workspace");
        for (const auto &part : relative) {
            require(part != "..", "path is outside workspace");
        }
        require(!relative.filename().empty(), "expected a file path");
        return relative;
    }

    void write_all(int fd, const char *data, size_t size) {
        while (size > 0) {
            const auto count = ::write(fd, data, size);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                system_error("write file");
            }
            data += count;
            size -= static_cast<size_t>(count);
        }
    }

    void read_file(int root, const fs::path &path, size_t limit) {
        File file(open_beneath(root, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        struct stat st{};
        if (fstat(file.fd, &st) != 0) {
            system_error("stat file");
        }
        require(S_ISREG(st.st_mode), "only regular files can be read");
        require(st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= limit,
                "file exceeds read limit");

        // Buffer before emitting stdout so failed reads never return partial data.
        std::string content;
        std::array<char, 16384> buffer{};
        for (;;) {
            const auto count = ::read(file.fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                system_error("read file");
            }
            if (count == 0) {
                break;
            }
            require(static_cast<size_t>(count) <= limit - content.size(),
                    "file exceeds read limit");
            content.append(buffer.data(), static_cast<size_t>(count));
        }
        write_all(STDOUT_FILENO, content.data(), content.size());
    }

    struct TemporaryFile {
        int parent;
        std::string name;
        bool installed = false;

        ~TemporaryFile() {
            if (!installed) {
                unlinkat(parent, name.c_str(), 0);
            }
        }
    };

    void write_file(int root, const fs::path &path, size_t limit) {
        File parent(open_beneath(root,
                                 path.has_parent_path() ? path.parent_path() : fs::path("."),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        const auto name = path.filename().string();
        mode_t mode = 0644;
        const int existing = openat(parent.fd, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (existing >= 0) {
            File target(existing);
            struct stat st{};
            if (fstat(target.fd, &st) != 0) {
                system_error("stat target");
            }
            require(S_ISREG(st.st_mode), "only regular files can be replaced");
            require(st.st_nlink == 1, "hard-linked target rejected");
            mode = st.st_mode & 0777;
        } else if (errno != ENOENT) {
            system_error("open target");
        }

        std::array<unsigned char, 16> random{};
        require(getrandom(random.data(), random.size(), 0) == ssize_t(random.size()),
                "cannot generate temporary filename");
        std::string temporary_name = ".sandbox-io-";
        for (auto byte : random) {
            temporary_name += "0123456789abcdef"[byte >> 4];
            temporary_name += "0123456789abcdef"[byte & 15];
        }
        File temporary(openat(parent.fd,
                              temporary_name.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              0600));
        TemporaryFile cleanup{parent.fd, temporary_name};
        std::array<char, 16384> buffer{};
        size_t total = 0;
        for (;;) {
            const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                system_error("read stdin");
            }
            if (count == 0) {
                break;
            }
            require(static_cast<size_t>(count) <= limit - total, "file exceeds write limit");
            write_all(temporary.fd, buffer.data(), static_cast<size_t>(count));
            total += static_cast<size_t>(count);
        }
        if (fchmod(temporary.fd, mode) != 0 || fsync(temporary.fd) != 0) {
            system_error("finish temporary file");
        }
        // Rename replaces the directory entry; it never follows a target symlink.
        if (renameat(parent.fd, temporary_name.c_str(), parent.fd, name.c_str()) != 0) {
            system_error("replace target");
        }
        cleanup.installed = true;
    }
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: sandbox-io read|write WORKSPACE PATH MAX_BYTES");
        const std::string operation = argv[1];
        const fs::path root = argv[2];
        const auto path = relative_file(root, argv[3]);
        const std::string limit_text = argv[4];
        require(!limit_text.empty() &&
                    limit_text.find_first_not_of("0123456789") == std::string::npos,
                "invalid file limit");
        const auto limit = std::stoull(limit_text);
        require(limit > 0 && limit <= 64 * 1024 * 1024, "invalid file limit");
        File workspace(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (operation == "read") {
            read_file(workspace.fd, path, limit);
        } else if (operation == "write") {
            write_file(workspace.fd, path, limit);
        } else {
            throw std::runtime_error("unknown file operation");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sandbox-io: " << error.what() << '\n';
        return 2;
    }
}
