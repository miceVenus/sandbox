#include "virtualization/environment/environment.hpp"
#include "lib/error.hpp"
#include "sandbox_types.hpp"

#include <fstream>

namespace fs = std::filesystem;

namespace virtualization::environment {
    namespace {
        const std::vector<fs::path> exported_directories = {
            "/usr/bin",   "/usr/lib", "/usr/lib64", "/usr/libexec", "/usr/include",
            "/usr/share", "/bin",     "/lib",       "/lib64"};

        auto beneath(const fs::path &path, const fs::path &directory) -> bool {
            const auto relative = path.lexically_relative(directory);
            if (relative.empty() || relative.is_absolute()) {
                return false;
            }
            for (const auto &part : relative) {
                if (part == "..") {
                    return false;
                }
            }
            return true;
        }

        auto exported_path(const fs::path &path) -> bool {
            for (const auto &directory : exported_directories) {
                if (beneath(path, directory)) {
                    return true;
                }
            }
            return false;
        }

        void write_config(const fs::path &path, const std::string &content) {
            std::ofstream out(path);
            out << content;
            out.close();
            lib::require(bool(out), "cannot write container configuration: " + path.string());
        }

        auto host_tool_mounts() -> std::vector<ToolMount> {
            std::vector<ToolMount> mounts;
            for (const auto &directory : exported_directories) {
                const auto type = fs::symlink_status(directory).type();
                if (type == fs::file_type::directory) {
                    mounts.push_back({directory, directory});
                } else if (type != fs::file_type::not_found && type != fs::file_type::symlink) {
                    throw std::runtime_error("unsupported host tool directory: " +
                                             directory.string());
                }
            }
            lib::require(fs::is_directory("/usr/bin") && fs::is_directory("/usr/lib"),
                         "HostTools requires system tools under /usr/bin and /usr/lib");
            return mounts;
        }
    } // namespace

    auto make_config(const Options &options) -> Config {
        Config config;
        if (options.environment == Environment::HostTools) {
            config.tool_mounts = host_tool_mounts();
            config.variables = {"PATH=/env/python/bin:/usr/bin:/bin", "HOME=/env/home", "LANG=C",
                                "TMPDIR=/build/tmp", "XDG_CACHE_HOME=/cache",
                                "PIP_CACHE_DIR=/cache/pip", "PIP_REQUIRE_VIRTUALENV=true"};
        } else {
            config.variables = {"PATH=/bin:/usr/bin", "HOME=" + options.ctr_repo.string(),
                                "LANG=C"};
        }
        config.variables.insert(config.variables.end(),
                                {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
                                 "GIT_TERMINAL_PROMPT=0"});
        return config;
    }

    void validate_host_tools(const Config &config, const fs::path &source_repository,
                             const fs::path &manager_root) {
        for (const auto &mount : config.tool_mounts) {
            const auto source = fs::canonical(mount.source);
            for (const auto &private_path : {source_repository, manager_root}) {
                lib::require(!beneath(private_path, source) && !beneath(source, private_path),
                             "HostTools directory overlaps source repository or manager root: " +
                                 source.string());
            }
        }
    }

    void prepare_host_tools(const fs::path &rootfs, const Config &config) {
        for (const auto &mount : config.tool_mounts) {
            fs::create_directories(rootfs / mount.destination.relative_path());
        }
        // Preserve merged-/usr layouts without importing the host's root directory.
        for (const auto &directory : exported_directories) {
            if (fs::is_symlink(fs::symlink_status(directory))) {
                const auto resolved = fs::canonical(directory);
                lib::require(exported_path(resolved),
                             "host tool alias points outside exported directories");
                const auto target = rootfs / directory.relative_path();
                fs::create_directories(target.parent_path());
                fs::create_symlink(fs::read_symlink(directory), target);
            }
        }

        fs::create_directories(rootfs / "etc/alternatives");
        if (fs::is_directory("/etc/alternatives")) {
            for (const auto &entry : fs::directory_iterator("/etc/alternatives")) {
                if (!entry.is_symlink()) {
                    continue;
                }
                std::error_code error;
                const auto target = fs::canonical(entry.path(), error);
                if (!error && exported_path(target)) {
                    fs::create_symlink(target,
                                       rootfs / "etc/alternatives" / entry.path().filename());
                }
            }
        }
        // Generated files only: never copy host passwd, credentials or service sockets.
        write_config(rootfs / "etc/passwd", "root:x:0:0:Sandbox:/env/home:/bin/sh\n"
                                            "sandbox:x:65534:65534:Sandbox:/env/home:/bin/sh\n");
        write_config(rootfs / "etc/group", "root:x:0:\nsandbox:x:65534:\n");
        write_config(rootfs / "etc/nsswitch.conf", "passwd: files\ngroup: files\nhosts: files\n");
        write_config(rootfs / "etc/hosts", "127.0.0.1 localhost\n::1 localhost\n");
    }
} // namespace virtualization::environment
