#include "virtualization/runtime_files.hpp"
#include "lib.hpp"
#include <sstream>
namespace fs = std::filesystem;

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
    const Result dependencies = run_process({"/usr/bin/ldd", executable.string()}, 10000);
    checked(dependencies);
    require(dependencies.out.find("=> not found") == std::string::npos,
            "SDK runtime dependency is missing: " + dependencies.out);
    std::istringstream words(dependencies.out);
    std::string word;
    while (words >> word) {
        if (!word.empty() && word.front() == '/') {
            copy_host_file(word);
        }
    }
}
