#pragma once

#include <stdexcept>
#include <string>

namespace lib {
    void require(bool condition, const std::string &message);
    [[noreturn]] void system_error(const char *operation);
} // namespace lib
