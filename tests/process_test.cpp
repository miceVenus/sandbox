#include "lib/memory_file.hpp"
#include "lib/process.hpp"
#include "test_support.hpp"
#include <fcntl.h>
#include <iostream>

auto main() -> int {
    try {
        const auto normal =
            lib::run_process({"/bin/sh", "-c", "printf out; printf err >&2; exit 7"}, 1000);
        test::check(normal.out == "out" && normal.err == "err" && normal.runtime_status == 7,
                    "stdout/stderr or exit status lost");
        test::check(lib::run_process({"/bin/sh", "-c", "sleep 10"}, 50).timed_out,
                    "process timeout failed");
        const auto flooding =
            lib::run_process({"/bin/sh", "-c", "while :; do echo x; done"}, 1000, 1024);
        test::check(flooding.output_limited && flooding.out.size() <= 1024, "output limit failed");
        std::string payload(1024 * 1024, 'x');
        payload[0] = '\0';
        payload[1] = char(255);
        const auto echo = lib::run_process({"/bin/cat"}, 2000, payload.size(), true, payload);
        test::check(echo.runtime_status == 0 && echo.out == payload && !echo.timed_out &&
                        !echo.output_limited,
                    "binary stdin failed");
        const auto closed =
            lib::run_process({"/bin/sh", "-c", "exec 0<&-; exit 0"}, 1000, 1024, true, payload);
        test::check(closed.runtime_status == 0 && !closed.timed_out, "closed stdin failed");
        test::check(lib::run_process({"/bin/sleep", "10"}, 50, 1024, true, payload).timed_out,
                    "blocked stdin timeout failed");
        auto inherited = lib::sealed_memory_file("process-test", "inherited bytes");
        lib::UniqueFd hidden(fcntl(inherited.get(), F_DUPFD, 60));
        test::check(hidden.get() >= 60, "could not create unlisted descriptor");
        lib::ProcessSupervision supervision;
        supervision.inherited_fds.push_back(inherited.get());
        const auto mapped = lib::run_process(
            {"/bin/sh", "-c",
             "test ! -e /proc/self/fd/" + std::to_string(hidden.get()) + " || exit 9; cat <&3"},
            1000, 1024, true, {}, supervision);
        test::check(mapped.runtime_status == 0 && mapped.out == "inherited bytes",
                    "explicit descriptor mapping failed");
        std::cout << "process supervision tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
