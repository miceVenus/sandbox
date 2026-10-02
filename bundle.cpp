#include "bundle.hpp"
#include "host_tools.hpp"
#include "lib.hpp"
#include "oci.hpp"
#include "resources.hpp"
#include "sandbox.hpp"

#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    void install_program(const fs::path &rootfs,
                         const fs::path &executable,
                         const fs::path &container_path) {

        require(executable.is_absolute() && fs::is_regular_file(executable),
                "host executable is unavailable: " + executable.string());

        auto copy_host_file = [&](const fs::path &source) {
            require(source.is_absolute() && fs::is_regular_file(source),
                    "invalid runtime dependency: " + source.string());
            const fs::path target = rootfs / source.relative_path();
            fs::create_directories(target.parent_path());
            fs::copy_file(source, target, fs::copy_options::skip_existing);
        };

        const auto target = rootfs / container_path.relative_path();
        fs::create_directories(target.parent_path());
        fs::copy_file(executable, target);
        const Result dependencies = run_process({"/usr/bin/ldd", executable.string()}, 10000);
        checked(dependencies);
        std::istringstream words(dependencies.out);
        std::string word;
        while (words >> word) {
            if (!word.empty() && word.front() == '/') {
                copy_host_file(word);
            }
        }
    }

} // namespace

void prepare_bundle(const Session &s) {
    const auto &o = s.options;
    const auto helper = sandbox_resources::file_helper_path();
    require(fs::is_regular_file(helper) && access(helper.c_str(), X_OK) == 0,
            "SDK file helper missing or not executable: " + helper.string() +
                "; build or install the SDK resources");
    fs::create_directories(s.bundle_dir / "rootfs");
    const auto root = s.bundle_dir / "rootfs";
    for (const auto &dir : {"proc", "dev", "dev/pts", "dev/shm", "tmp", "sandbox-tools"}) {
        fs::create_directories(root / dir);
    }
    fs::create_directories(root / o.ctr_repo.relative_path());
    if (o.environment == Environment::HostTools) {
        prepare_host_tools(root);
        fs::copy_file(helper,
                      root / fs::path(sandbox_resources::container_file_helper).relative_path());
        for (const auto &directory : {"build", "env", "cache"}) {
            fs::create_directories(root / directory);
            fs::create_directories(s.s_dir / "runtime-data" / directory);
        }
        fs::create_directories(s.s_dir / "runtime-data/build/tmp");
        fs::create_directories(s.s_dir / "runtime-data/env/home");
        fs::create_directories(s.s_dir / "runtime-data/cache/pip");
    } else {
        fs::create_directories(root / "bin");
        const auto file = run_process({"/usr/bin/file", "/usr/bin/busybox"}, 10000);
        require(file.runtime_status == 0 &&
                    (file.out.find("statically linked") != std::string::npos ||
                     file.out.find("static-pie linked") != std::string::npos),
                "install a static busybox first");
        fs::copy_file("/usr/bin/busybox", root / "bin/busybox");
        const auto applets = run_process({"/usr/bin/busybox", "--list"}, 10000);
        checked(applets);
        std::istringstream lines(applets.out);
        std::string applet;
        while (std::getline(lines, applet)) {
            if (!applet.empty()) {
                require(applet.find('/') == std::string::npos, "invalid busybox applet");
                if (applet != "busybox") {
                    fs::create_symlink("busybox", root / "bin" / applet);
                }
            }
        }
        install_program(root, "/usr/bin/git", "/usr/bin/git");
        install_program(root, helper, sandbox_resources::container_file_helper);
    }

    const bool rootless = geteuid() != 0;
    if (!rootless) {
        require(lchown(s.work_files_dir.c_str(), 65534, 65534) == 0, "cannot set task ownership");
        for (auto &entry : fs::recursive_directory_iterator(s.work_files_dir)) {
            require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                    "cannot set task file ownership");
        }
        if (o.environment == Environment::HostTools) {
            for (auto &entry : fs::recursive_directory_iterator(s.s_dir / "runtime-data")) {
                require(lchown(entry.path().c_str(), 65534, 65534) == 0,
                        "cannot set runtime data ownership");
            }
        }
    }

    prepare_oci_config(s);
}
