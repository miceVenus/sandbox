#include "lib/memory_file.hpp"
#include "lib/process.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <sys/wait.h>

auto main() -> int {
    try {
        lib::ProcessOptions normal_options;
        normal_options.timeout_ms = 1000;
        const auto normal = lib::run_process(
            {"/bin/sh", "-c", "printf out; printf err >&2; exit 7"}, normal_options);
        test::check(normal.out == "out" && normal.err == "err" && normal.runtime_status == 7,
                    "stdout/stderr or exit status lost");

        lib::ProcessOptions short_options;
        short_options.timeout_ms = 50;
        test::check(lib::run_process({"/bin/sh", "-c", "sleep 10"}, short_options).timed_out,
                    "process timeout failed");
        auto limited_options = normal_options;
        limited_options.output_limit = 1024;
        const auto flooding =
            lib::run_process({"/bin/sh", "-c", "while :; do echo x; done"}, limited_options);
        test::check(flooding.output_limited && flooding.out.size() <= 1024, "output limit failed");

        std::string payload(1024 * 1024, 'x');
        payload[0] = '\0';
        payload[1] = char(255);
        auto input_options = normal_options;
        input_options.timeout_ms = 2000;
        input_options.output_limit = payload.size();
        input_options.stdin_data = payload;
        const auto echo = lib::run_process({"/bin/cat"}, input_options);
        test::check(echo.runtime_status == 0 && echo.out == payload && !echo.timed_out &&
                        !echo.output_limited,
                    "binary stdin failed");
        const auto closed =
            lib::run_process({"/bin/sh", "-c", "exec 0<&-; exit 0"}, input_options);
        test::check(closed.runtime_status == 0 && !closed.timed_out, "closed stdin failed");
        short_options.stdin_data = payload;
        test::check(lib::run_process({"/bin/sleep", "10"}, short_options).timed_out,
                    "blocked stdin timeout failed");

        auto inherited = lib::sealed_memory_file("process-test", "inherited bytes");
        auto second = lib::sealed_memory_file("process-test-second", "second bytes");
        lib::UniqueFd hidden(fcntl(inherited.get(), F_DUPFD, 60));
        test::check(hidden.get() >= 60, "could not create unlisted descriptor");
        auto inherited_options = normal_options;
        // Reversing the sources exercises overlapping mappings to child FDs 3 and 4.
        inherited_options.inherited_fds = {second.get(), inherited.get()};
        const auto mapped = lib::run_process(
            {"/bin/sh", "-c",
             "test ! -e /proc/self/fd/" + std::to_string(hidden.get()) +
                 " || exit 9; cat <&3; cat <&4"},
            inherited_options);
        test::check(mapped.runtime_status == 0 && mapped.out == "second bytesinherited bytes",
                    "explicit descriptor mapping failed");

        test::TemporaryDirectory temporary;
        const auto host_cwd = std::filesystem::current_path();
        auto environment_options = normal_options;
        environment_options.cwd = temporary.path;
        environment_options.environment = {"PATH=/usr/bin:/bin", "LANG=C", "SANDBOX_VALUE=a b=c"};
        const auto environment = lib::run_process(
            {"/bin/sh", "-c", "printf '%s|%s' \"$PWD\" \"$SANDBOX_VALUE\"; test -z \"$HOME\""},
            environment_options);
        test::check(environment.runtime_status == 0 &&
                        environment.out == temporary.path.string() + "|a b=c" &&
                        std::filesystem::current_path() == host_cwd,
                    "child cwd or explicit environment lost");

        std::atomic<bool> cancel{false};
        auto cancel_options = normal_options;
        cancel_options.cancel = &cancel;
        cancel_options.on_output = [&](bool, std::string_view) { cancel = true; };
        const auto cancelled =
            lib::run_process({"/bin/sh", "-c", "printf ready; exec /bin/sleep 10"}, cancel_options);
        test::check(cancelled.cancelled && !cancelled.timed_out && cancelled.out == "ready",
                    "callback-triggered cancellation failed");
        Result cancelled_success;
        cancelled_success.runtime_status = 0;
        cancelled_success.cancelled = true;
        test::rejects([&] { lib::check_process_result(cancelled_success); });

        // A callback failure must kill and reap the child before propagating the exception.
        pid_t callback_pid = -1;
        std::string pid_bytes;
        auto callback_options = normal_options;
        callback_options.on_output = [&](bool, std::string_view bytes) {
            pid_bytes.append(bytes);
            if (pid_bytes.find('\n') != std::string::npos) {
                callback_pid = static_cast<pid_t>(std::stol(pid_bytes));
                throw std::runtime_error("output callback failed");
            }
        };
        test::check(test::rejects([&] {
                        lib::run_process(
                            {"/bin/sh", "-c", "printf '%s\\n' \"$$\"; exec /bin/sleep 10"},
                            callback_options);
                    }) == "output callback failed",
                    "callback error was lost");
        test::check(callback_pid > 0 && waitpid(callback_pid, nullptr, WNOHANG) == -1 &&
                        errno == ECHILD,
                    "callback failure left an unreaped child");

        // Startup failures must release pipes, socketpair and copied inherited descriptors.
        auto fd_count = [] {
            return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                 std::filesystem::directory_iterator());
        };
        const auto descriptors_before = fd_count();
        auto failed_options = inherited_options;
        failed_options.stdin_data = payload;
        test::rejects([&] {
            lib::run_process({(temporary.path / "missing-program").string()}, failed_options);
        });
        failed_options.cwd = temporary.path / "missing-directory";
        test::rejects([&] { lib::run_process({"/bin/true"}, failed_options); });
        failed_options.cwd.clear();
        failed_options.environment = {"INVALID"};
        test::rejects([&] { lib::run_process({"/bin/true"}, failed_options); });
        test::check(fd_count() == descriptors_before, "startup failure leaked descriptors");

        // Detached children can retain the pipes after the leader exits; do not wait for EOF.
        auto detached_options = normal_options;
        detached_options.drain_until_eof = false;
        detached_options.kill_remaining_group = true;
        const auto detached =
            lib::run_process({"/bin/sh", "-c", "sleep 10 & printf detached"}, detached_options);
        test::check(detached.runtime_status == 0 && !detached.timed_out &&
                        detached.out == "detached",
                    "leader exit waited for a detached child's pipes");
        std::cout << "process supervision tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
