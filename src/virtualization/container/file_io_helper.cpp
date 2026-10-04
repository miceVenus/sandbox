// Internal OCI helper. All file access happens inside the container.
#include "../../../workspace_files.hpp"
#include <array>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char **argv) {
    try {
        if (argc != 5) {
            throw std::runtime_error("usage: sandbox-io read|write WORKSPACE PATH MAX_BYTES");
        }
        const std::string operation = argv[1];
        const std::string text = argv[4];
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
            throw std::runtime_error("invalid file limit");
        }
        const auto limit = std::stoull(text);
        WorkspaceFiles files(argv[2]);
        if (operation == "read") {
            const auto content = files.read(argv[3], limit);
            std::cout.write(content.data(), content.size());
            if (!std::cout) {
                throw std::runtime_error("write helper stdout failed");
            }
        } else if (operation == "write") {
            auto writer = files.begin_write(argv[3], limit);
            std::array<char, 16384> buffer{};
            for (;;) {
                const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0) {
                    throw std::runtime_error("read helper stdin failed");
                }
                if (count == 0) {
                    break;
                }
                writer->append({buffer.data(), static_cast<size_t>(count)});
            }
            writer->commit();
        } else {
            throw std::runtime_error("unknown file operation");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sandbox-io: " << error.what() << '\n';
        return 2;
    }
}
