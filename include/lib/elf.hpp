#pragma once
#include <filesystem>
#include <string_view>
#include <vector>

namespace lib {
    auto dynamic_dependency_paths(std::string_view output) -> std::vector<std::filesystem::path>;
}
