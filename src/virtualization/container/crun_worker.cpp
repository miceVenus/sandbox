#include "virtualization/container/crun_worker_protocol.hpp"

#include <cerrno>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    using nlohmann::json;

    json read_request() {
        // Consume and close control before libcrun touches descriptors or starts
        // a task. Task stdin, including binary file contents, stays independent.
        struct ControlFd {
            ~ControlFd() { close(crun_worker::control_fd); }
        } control;
        struct stat st{};
        if (fstat(crun_worker::control_fd, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > crun_worker::max_request_bytes) {
            throw std::runtime_error("invalid libcrun control descriptor or request size");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = pread(crun_worker::control_fd, bytes.data() + offset,
                                     bytes.size() - offset, static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error("incomplete libcrun control request");
            }
            offset += static_cast<std::size_t>(count);
        }
        return json::from_cbor(bytes);
    }

    std::string checked_string(const json &message, const char *name, bool absolute = false) {
        auto value = message.at(name).get<std::string>();
        if (value.find('\0') != std::string::npos ||
            (absolute && (value.empty() || value.front() != '/'))) {
            throw std::runtime_error(std::string("invalid libcrun request field: ") + name);
        }
        return value;
    }

    void configure_host_environment(const json &message) {
        const auto &environment = message.at("host_environment");
        const auto runtime = checked_string(environment, "runtime_directory", true);
        const auto home = checked_string(environment, "home", true);
        const auto bus = "unix:path=" + runtime + "/bus";
        if (setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1) < 0 ||
            setenv("DBUS_SESSION_BUS_ADDRESS", bus.c_str(), 1) < 0 ||
            setenv("HOME", home.c_str(), 1) < 0) {
            throw std::runtime_error("cannot configure libcrun host connection");
        }
    }
} // namespace

// Private SDK worker: argv contains only the executable name, never runtime flags.
int main(int argc, char **argv) {
    try {
        if (argc != 1) {
            throw std::runtime_error("libcrun worker accepts only its private control request");
        }
        const auto message = read_request();
        if (message.at("version") != crun_worker::protocol_version ||
            !message.at("operation").is_number_integer()) {
            throw std::runtime_error("unsupported libcrun worker protocol");
        }
        const auto operation = message.at("operation").get<int>();
        if (operation < SANDBOX_CRUN_START || operation > SANDBOX_CRUN_DELETE) {
            throw std::runtime_error("unknown libcrun operation");
        }
        const auto root = checked_string(message, "state_root", true);
        const auto id = checked_string(message, "id");
        if (id.empty() || id.front() == '.' || id.find('/') != std::string::npos) {
            throw std::runtime_error("invalid libcrun container ID");
        }
        const auto bundle = checked_string(message, "bundle", operation == SANDBOX_CRUN_START);
        const bool has_listener = message.at("agent_listener").get<bool>();
        if (has_listener) {
            int accepting = 0;
            socklen_t size = sizeof(accepting);
            if (operation != SANDBOX_CRUN_START ||
                getsockopt(4, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) < 0 || !accepting ||
                dup2(4, 3) < 0) {
                throw std::runtime_error("invalid container agent listener");
            }
            close(4);
        }
        const bool systemd = message.at("systemd_cgroups").get<bool>();
        if (systemd) {
            configure_host_environment(message);
        }
        const sandbox_crun_request request{static_cast<sandbox_crun_operation>(operation),
                                           root.c_str(), id.c_str(), bundle.c_str(), systemd, has_listener ? 1 : 0};
        return sandbox_libcrun_dispatch(&request, argc, argv);
    } catch (const std::exception &error) {
        std::cerr << "libcrun worker: " << error.what() << '\n';
        return 125;
    }
}
