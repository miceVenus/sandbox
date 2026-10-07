#include "lib/elf.hpp"
#include "lib/error.hpp"
#include <sstream>

namespace lib {
    namespace fs = std::filesystem;
    auto dynamic_dependency_paths(std::string_view output) -> std::vector<fs::path> {
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
                require(line != "not found",
                        "dynamic dependency is missing: " + std::string(output));
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

} // namespace lib
