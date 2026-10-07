#pragma once

#include <string>
#include <string_view>

namespace lib {
    auto trim_newlines(std::string value) -> std::string;
    auto random_id() -> std::string;
    auto is_hex(std::string_view value) -> bool;
} // namespace lib
