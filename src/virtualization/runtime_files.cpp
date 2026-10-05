#include "virtualization/runtime_files.hpp"
#include "lib.hpp"
#include <sstream>
namespace fs = std::filesystem;

std::vector<fs::path> runtime_files::dynamic_dependency_paths(std::string_view output) {
    std::vector<fs::path> paths;
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        line.erase(0, first);
        const auto separator = line.find(" => ");
        if (separator != std::string::npos) {
            line.erase(0, separator + 4);
            require(line != "not found", "SDK runtime dependency is missing: " + std::string(output));
        }
        if (line.empty() || line.front() != '/') {
            continue; // linux-vdso and other entries without a filesystem path.
        }
        // The final load address is a field delimiter, unlike spaces inside the path.
        const auto address = line.rfind(" (0x");
        if (address != std::string::npos) {
            line.resize(address);
        }
        paths.emplace_back(line);
    }
    return paths;
}

void install_runtime_program(const fs::path &rootfs,
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
    const auto private_libraries = executable.parent_path() / "lib";
    bool needs_private_libraries = false;
    const Result dependencies = run_process({"/usr/bin/ldd", executable.string()}, 10000);
    checked(dependencies);
    require(dependencies.out.find("=> not found") == std::string::npos,
            "SDK runtime dependency is missing: " + dependencies.out);
    for (const auto &dependency : runtime_files::dynamic_dependency_paths(dependencies.out)) {
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
            require(entry.is_regular_file(), "invalid private runtime library");
            fs::copy_file(entry.path(), destination / entry.path().filename(), fs::copy_options::skip_existing);
        }
    }
}
