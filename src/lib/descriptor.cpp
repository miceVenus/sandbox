#include "lib/descriptor.hpp"

#include <unistd.h>
#include <utility>

namespace lib {
    UniqueFd::UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {
    }
    UniqueFd::~UniqueFd() {
        reset();
    }
    UniqueFd::UniqueFd(UniqueFd &&other) noexcept : descriptor_(other.release()) {
    }
    auto UniqueFd::operator=(UniqueFd &&other) noexcept -> UniqueFd & {
        if (this != &other) {
            reset();
            descriptor_ = other.release();
        }
        return *this;
    }

    auto UniqueFd::get() const noexcept -> int {
        return descriptor_;
    }

    auto UniqueFd::release() noexcept -> int {
        return std::exchange(descriptor_, -1);
    }
    void UniqueFd::reset() noexcept {
        const int descriptor = release();
        if (descriptor >= 0) {
            close(descriptor);
        }
    }
} // namespace lib
