#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace lib {
    void write_json(const std::filesystem::path &path, const nlohmann::json &value);
}
