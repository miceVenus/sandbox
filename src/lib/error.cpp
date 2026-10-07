#include "lib/error.hpp"

#include <cerrno>
#include <cstring>

namespace lib {
    void require(bool condition, const std::string &message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void system_error(const char *operation) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }
} // namespace lib
