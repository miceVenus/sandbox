#include "lib/memory_file.hpp"
#include "lib/error.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace lib {
    auto sealed_memory_file(std::string_view name, std::string_view bytes, int minimum_descriptor)
        -> UniqueFd {
        require(!name.empty() && name.find('\0') == std::string_view::npos,
                "invalid memory file name");
        UniqueFd original(memfd_create(std::string(name).c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (original.get() < 0) {
            system_error("create memory file");
        }
        UniqueFd descriptor(fcntl(original.get(), F_DUPFD_CLOEXEC, minimum_descriptor));
        if (descriptor.get() < 0) {
            system_error("duplicate memory file");
        }
        while (!bytes.empty()) {
            const auto count = write(descriptor.get(), bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                system_error("write memory file");
            }
            bytes.remove_prefix(static_cast<size_t>(count));
        }
        if (lseek(descriptor.get(), 0, SEEK_SET) < 0 ||
            fcntl(descriptor.get(), F_ADD_SEALS,
                  F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
            system_error("seal memory file");
        }
        return descriptor;
    }
} // namespace lib
