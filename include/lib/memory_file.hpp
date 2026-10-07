#pragma once
#include "lib/descriptor.hpp"
#include <string_view>

namespace lib {
    // An anonymous, immutable file. Its descriptor is safe to inherit explicitly.
    auto sealed_memory_file(std::string_view name, std::string_view bytes,
                            int minimum_descriptor = 3) -> UniqueFd;
} // namespace lib
