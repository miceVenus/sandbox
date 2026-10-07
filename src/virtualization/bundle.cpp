#include "virtualization/bundle.hpp"
#include "lib/error.hpp"
#include "lib/process.hpp"
#include "resources.hpp"
#include "sandbox.hpp"
#include "virtualization/host_tools.hpp"
#include "virtualization/oci.hpp"
#include "virtualization/rootfs_program.hpp"

#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

void prepare_bundle(const SandboxInfo &info) {
    const auto &options = info.options;
    fs::create_directories(info.bundle_dir / "rootfs");
    const auto root = info.bundle_dir / "rootfs";
    for (const auto &dir : {"proc", "dev", "dev/pts", "dev/shm", "tmp", "sandbox-tools"}) {
        fs::create_directories(root / dir);
    }
    fs::create_directories(root / options.ctr_repo.relative_path());
    if (options.environment == Environment::HostTools) {
        prepare_host_tools(root);
        for (const auto &directory : {"build", "env", "cache"}) {
            fs::create_directories(root / directory);
            fs::create_directories(info.directory / "runtime-data" / directory);
        }
        fs::create_directories(info.directory / "runtime-data/build/tmp");
        fs::create_directories(info.directory / "runtime-data/env/home");
        fs::create_directories(info.directory / "runtime-data/cache/pip");
    } else {
        fs::create_directories(root / "bin");
        const auto file = lib::run_process({"/usr/bin/file", "/usr/bin/busybox"}, 10000);
        lib::require(file.runtime_status == 0 &&
                         (file.out.find("statically linked") != std::string::npos ||
                          file.out.find("static-pie linked") != std::string::npos),
                     "install a static busybox first");
        fs::copy_file("/usr/bin/busybox", root / "bin/busybox");
        const auto applets = lib::run_process({"/usr/bin/busybox", "--list"}, 10000);
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
        install_runtime_program(root, "/usr/bin/git", "/usr/bin/git");
    }

    const auto agentd = sandbox_resources::agentd_path();
    lib::require(fs::is_regular_file(agentd) && access(agentd.c_str(), X_OK) == 0,
                 "SDK agentd missing or not executable: " + agentd.string());
    install_runtime_program(root, agentd, "/sandbox-tools/agentd");

    const bool rootless = geteuid() != 0;
    if (!rootless) {
        lib::require(lchown(info.work_files_dir.c_str(), 65534, 65534) == 0,
                     "cannot set task ownership");
        for (auto &entry : fs::recursive_directory_iterator(info.work_files_dir)) {
            lib::require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                         "cannot set task file ownership");
        }
        if (options.environment == Environment::HostTools) {
            for (auto &entry : fs::recursive_directory_iterator(info.directory / "runtime-data")) {
                lib::require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                             "cannot set runtime data ownership");
            }
        }
    }

    prepare_oci_config(info);
}
