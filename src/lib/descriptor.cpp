#include "lib/descriptor.hpp"

#include <unistd.h>
#include <utility>

namespace lib {
    UniqueFd::UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {
    }
    UniqueFd::~UniqueFd() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }
    UniqueFd::UniqueFd(UniqueFd &&other) noexcept : descriptor_(other.release()) {
    }
    auto UniqueFd::operator=(UniqueFd &&other) noexcept -> UniqueFd & {
        if (this != &other) {
            if (descriptor_ >= 0) {
                close(descriptor_);
            }
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
} // namespace lib
