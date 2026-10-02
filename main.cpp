// Linux-only educational CLI. This is not a privileged service API.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "client.hpp"
#include "process.hpp"
#include "sandbox.hpp"
#include "workspace.hpp"

void validate_id(const std::string &id) {
    if (id.empty() || id.size() > 64 || id[0] == '-') {
        throw std::runtime_error("invalid ID");
    }
    for (unsigned char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            throw std::runtime_error("invalid ID");
        }
    }
}

int main(int argc, char **argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "workspace") {
            return workspace_cli(argc - 2, argv + 2);
        }
        if (argc >= 2 && std::string(argv[1]) == "session") {
            return session_cli(argc - 2, argv + 2);
        }
        // Exercises subprocess supervision without containers or root.
        if (argc == 2 && std::string(argv[1]) == "self-test") {
            const auto normal =
                run_process({"/bin/sh", "-c", "printf out; printf err >&2; exit 7"}, 1000);
            const auto timeout = run_process({"/bin/sh", "-c", "sleep 10"}, 50);
            const auto flooding =
                run_process({"/bin/sh", "-c", "while :; do echo x; done"}, 1000, 1024);
            std::string payload(1024 * 1024, 'x');
            payload[0] = '\0';
            payload[1] = char(255);
            const auto echo = run_process({"/bin/cat"}, 2000, payload.size(), true, payload);
            const auto closed_input =
                run_process({"/bin/sh", "-c", "exec 0<&-; exit 0"}, 1000, 1024, true, payload);
            const auto blocked_input = run_process({"/bin/sleep", "10"}, 50, 1024, true, payload);
            if (normal.out != "out" || normal.err != "err" || normal.runtime_status != 7 ||
                !timeout.timed_out || !flooding.output_limited || flooding.out.size() > 1024) {
                throw std::runtime_error("self-test failed");
            }
            if (echo.runtime_status != 0 || echo.out != payload || echo.timed_out ||
                echo.output_limited || closed_input.runtime_status != 0 || closed_input.timed_out ||
                !blocked_input.timed_out) {
                throw std::runtime_error("stdin supervision test failed");
            }
            std::cout << "self-test passed\n";
            return 0;
        }
        if (argc < 3) {
            throw std::runtime_error(
                "usage: sandboxctl start ID BUNDLE | exec ID TIMEOUT_MS /absolute/cmd [args...] | "
                "stop ID | state ID | workspace ...");
        }
        const std::string action = argv[1], id = argv[2];
        validate_id(id);
        CrunClient runtime;
        Result result;
        if (action == "start" && argc == 4) {
            result = runtime.start(id, argv[3]);
            // Only a timed-out launch is automatically cleaned; a generic
            // launch error could mean the ID already belonged to another task.
            if (result.timed_out || result.output_limited) {
                runtime.destroy(id);
            }
        } else if (action == "exec" && argc >= 5) {
            size_t consumed = 0;
            const int ms = std::stoi(argv[3], &consumed);
            if (consumed != std::strlen(argv[3]) || ms < 1 || ms > 3600000) {
                throw std::runtime_error("invalid timeout");
            }
            std::vector<std::string> command(argv + 4, argv + argc);
            if (command.front().empty() || command.front()[0] != '/') {
                throw std::runtime_error("command must use an absolute container path");
            }
            result = runtime.exec(id, "/workspace", command, ms);
            if (result.timed_out || result.output_limited) {
                if (!runtime.destroy(id)) {
                    return 125;
                }
                std::cerr << "session destroyed; workspace retained\n";
            }
        } else if (action == "stop" && argc == 3) {
            return runtime.destroy(id) ? 0 : 125;
        } else if (action == "state" && argc == 3) {
            result = runtime.state(id);
        } else {
            throw std::runtime_error("invalid arguments");
        }

        std::cout << result.out;
        std::cerr << result.err;
        if (result.timed_out) {
            std::cerr << "timeout\n";
            return 124;
        }
        if (result.output_limited) {
            std::cerr << "output limit\n";
            return 125;
        }
        return result.runtime_status;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 125;
    }
}
