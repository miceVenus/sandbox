#include "virtualization/rootfs_program.hpp"
#include "lib/elf.hpp"
#include "lib/error.hpp"
#include "lib/process.hpp"
#include <sstream>

namespace fs = std::filesystem;

void install_runtime_program(const fs::path &rootfs, const fs::path &executable,
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
    const Result dependencies = lib::run_process({"/usr/bin/ldd", executable.string()}, 10000);
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
