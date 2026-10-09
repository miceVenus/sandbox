#include "virtualization/environment/environment.hpp"
#include "lib/elf.hpp"
#include "lib/error.hpp"
#include "lib/process.hpp"
#include "resources.hpp"
#include "sandbox_types.hpp"

#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace virtualization::environment {
    namespace {
        void install_busybox(const fs::path &root) {
            fs::create_directories(root / "bin");
            const auto file = lib::run_process({"/usr/bin/file", "/usr/bin/busybox"});
            lib::require(
                file.runtime_status == 0 &&
                    (file.out.find("statically linked") != std::string::npos ||
                     file.out.find("static-pie linked") != std::string::npos),
                "install a static busybox first");

            fs::copy_file("/usr/bin/busybox", root / "bin/busybox");
            const auto applets = lib::run_process({"/usr/bin/busybox", "--list"});
            lib::check_process_result(applets);
            std::istringstream lines(applets.out);
            std::string applet;
            while (std::getline(lines, applet)) {
                if (!applet.empty()) {
                    lib::require(applet.find('/') == std::string::npos, "invalid busybox applet");
                    if (applet != "busybox") {
                        fs::create_symlink("busybox", root / "bin" / applet);
                    }
                }
            }
        }
    } // namespace

    void install_program(const fs::path &rootfs, const fs::path &executable,
                         const fs::path &container_path) {
        lib::require(executable.is_absolute() && fs::is_regular_file(executable),
                     "host executable is unavailable: " + executable.string());

        auto copy_host_file = [&](const fs::path &source) {
            lib::require(source.is_absolute() && fs::is_regular_file(source),
                         "invalid runtime dependency: " + source.string());
            const fs::path target = rootfs / source.relative_path();
            fs::create_directories(target.parent_path());
            fs::copy_file(source, target, fs::copy_options::skip_existing);
        };

        const auto target = rootfs / container_path.relative_path();
        fs::create_directories(target.parent_path());
        fs::copy_file(executable, target);
        const auto private_libraries = executable.parent_path() / "lib";
        bool needs_private_libraries = false;
        const Result dependencies = lib::run_process({"/usr/bin/ldd", executable.string()});
        lib::check_process_result(dependencies);
        lib::require(dependencies.out.find("=> not found") == std::string::npos,
                     "SDK runtime dependency is missing: " + dependencies.out);
        for (const auto &dependency : lib::dynamic_dependency_paths(dependencies.out)) {
            if (fs::is_directory(private_libraries) &&
                fs::canonical(dependency).parent_path() == fs::canonical(private_libraries)) {
                needs_private_libraries = true;
            } else {
                copy_host_file(dependency);
            }
        }
        if (needs_private_libraries) {
            // Preserve $ORIGIN/lib after relocating the VMM helper. Include firmware
            // loaded at runtime, but do not duplicate these libraries into agentd's rootfs.
            const auto destination = target.parent_path() / "lib";
            fs::create_directories(destination);
            for (const auto &entry : fs::directory_iterator(private_libraries)) {
                lib::require(entry.is_regular_file(), "invalid private runtime library");
                fs::copy_file(entry.path(), destination / entry.path().filename(),
                              fs::copy_options::skip_existing);
            }
        }
    }

    void prepare_runtime_data(const SandboxInfo &info) {
        const auto root = info.bundle_dir / "rootfs";
        const auto data = info.directory / "runtime-data";
        for (const auto *directory : runtime_directories) {
            fs::create_directories(root / directory);
            fs::create_directories(data / directory);
        }
        for (const auto *directory : {"build/tmp", "env/home", "cache/pip"}) {
            fs::create_directories(data / directory);
        }
    }

    void prepare_rootfs(const SandboxInfo &info, const Config &config) {
        const auto &options = info.options;
        const auto root = info.bundle_dir / "rootfs";
        fs::create_directories(root);
        for (const auto &dir : {"proc", "dev", "dev/pts", "dev/shm", "tmp", "sandbox-tools"}) {
            fs::create_directories(root / dir);
        }

        fs::create_directories(root / options.ctr_repo.relative_path());

        if (options.environment == Environment::HostTools) {
            prepare_host_tools(root, config);
            prepare_runtime_data(info);
        } else {
            install_busybox(root);
            install_program(root, "/usr/bin/git", "/usr/bin/git");
        }

        // agentd == 存在于容器中的服务进程的位置
        const auto agentd = sandbox_resources::agentd_path();
        lib::require(fs::is_regular_file(agentd) && access(agentd.c_str(), X_OK) == 0,
                     "SDK agentd missing or not executable: " + agentd.string());
        install_program(root, agentd, "/sandbox-tools/agentd");

        if (!info.rootless) {
            // root 模式下创建的workspace属于root所以 agentd有可能无法使用 所以需要转移所属权限
            lib::require(lchown(info.work_files_dir.c_str(), 65534, 65534) == 0,
                         "cannot set task ownership");

            for (auto &entry : fs::recursive_directory_iterator(info.work_files_dir)) {
                lib::require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                             "cannot set task file ownership");
            }

            if (options.environment == Environment::HostTools) {
                const auto data = info.directory / "runtime-data";
                for (auto &entry : fs::recursive_directory_iterator(data)) {
                    lib::require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                                 "cannot set runtime data ownership");
                }
            }
        }
    }
} // namespace virtualization::environment
