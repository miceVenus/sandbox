#include "lib/filesystem.hpp"
#include "lib/error.hpp"

#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace lib {
    void require_directory(const std::filesystem::path &path) {
        require(std::filesystem::symlink_status(path).type() ==
                    std::filesystem::file_type::directory,
                "expected a real directory: " + path.string());
    }

    void validate_relative_path(const std::filesystem::path &path) {
        require(!path.empty() && !path.is_absolute(), "expected a relative path");
        for (const auto &part : path) {
            require(part != "..", "relative path traversal rejected");
        }
        require(path.string().find('\0') == std::string::npos, "NUL in path");
    }

    auto resolve_relative_path(const std::filesystem::path &root,
                               const std::filesystem::path &relative) -> std::filesystem::path {
        validate_relative_path(relative);
        return (root / relative).lexically_normal();
    }

    auto open_beneath(int directory, const std::filesystem::path &path, int flags) -> int {
        open_how options{};
        options.flags = flags;
        options.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
        return syscall(SYS_openat2, directory, path.c_str(), &options, sizeof(options));
    }
} // namespace lib
