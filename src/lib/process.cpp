#include "lib/process.hpp"
#include "lib/descriptor.hpp"
#include "lib/error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace lib {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr int io_batch_operations = 16;
        constexpr size_t stdin_chunk_bytes = 16 * 1024;
        constexpr size_t output_chunk_bytes = 4096;
        // Cancellation uses an atomic flag; check it at least once per poll interval.
        constexpr int cancellation_poll_ms = 10;

        struct Pipe {
            UniqueFd read_end;
            UniqueFd write_end;

            Pipe() {
                int fds[2];
                if (pipe2(fds, O_CLOEXEC) < 0) {
                    system_error("create output pipe");
                }
                read_end = UniqueFd(fds[0]);
                write_end = UniqueFd(fds[1]);
                // Only the parent's read end is nonblocking; child writes remain blocking.
                if (fcntl(read_end.get(), F_SETFL, O_NONBLOCK) < 0) {
                    system_error("configure output pipe");
                }
            }
        };

        // MSG_NOSIGNAL avoids changing the caller's global SIGPIPE handling.
        struct InputStream {
            UniqueFd child;
            UniqueFd parent;

            explicit InputStream(bool enabled) {
                if (enabled) {
                    int fds[2];
                    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
                        system_error("create stdin socketpair");
                    }
                    child = UniqueFd(fds[0]);
                    parent = UniqueFd(fds[1]);
                }
            }
        };

        void check_spawn(int error, const char *operation) {
            // posix_spawn functions return the error directly, rather than setting errno.
            if (error) {
                throw std::runtime_error(std::string(operation) + ": " + std::strerror(error));
            }
        }

        struct SpawnSetup {
            posix_spawn_file_actions_t actions;
            posix_spawnattr_t attributes;

            SpawnSetup() {
                check_spawn(posix_spawn_file_actions_init(&actions), "initialize spawn actions");
                const int error = posix_spawnattr_init(&attributes);
                if (error) {
                    posix_spawn_file_actions_destroy(&actions);
                    check_spawn(error, "initialize spawn attributes");
                }
            }
            ~SpawnSetup() {
                posix_spawnattr_destroy(&attributes);
                posix_spawn_file_actions_destroy(&actions);
            }
            SpawnSetup(const SpawnSetup &) = delete;
            auto operator=(const SpawnSetup &) -> SpawnSetup & = delete;
        };

        auto spawn_process(const std::vector<std::string> &args, const ProcessOptions &options,
                           const Pipe &out, const Pipe &err, const InputStream &input) -> pid_t {
            require(!args.empty(), "empty argv");
            require(options.inherited_fds.size() <= 64, "too many inherited descriptors");
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const auto &arg : args) {
                argv.push_back(const_cast<char *>(arg.c_str()));
            }
            argv.push_back(nullptr);

            char path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
            char locale[] = "LANG=C";
            std::vector<char *> env;
            if (options.environment.empty()) {
                env = {path, locale};
            } else {
                for (const auto &entry : options.environment) {
                    require(entry.find('=') != std::string::npos && entry.front() != '=' &&
                                entry.find('\0') == std::string::npos,
                            "invalid launcher environment");
                    env.push_back(const_cast<char *>(entry.c_str()));
                }
            }
            env.push_back(nullptr);

            // Copy above all child targets before mapping overlapping descriptor numbers.
            const int first_unused = 3 + static_cast<int>(options.inherited_fds.size());
            std::vector<UniqueFd> inherited_copies;
            inherited_copies.reserve(options.inherited_fds.size());
            for (const int descriptor : options.inherited_fds) {
                require(descriptor >= 3, "inherited descriptor must be >= 3");
                UniqueFd copy(fcntl(descriptor, F_DUPFD_CLOEXEC, first_unused));
                if (copy.get() < 0) {
                    system_error("duplicate inherited descriptor");
                }
                inherited_copies.push_back(std::move(copy));
            }

            SpawnSetup setup;
            if (!options.cwd.empty()) {
                check_spawn(
                    posix_spawn_file_actions_addchdir_np(&setup.actions, options.cwd.c_str()),
                    "set child working directory");
            }
            if (input.child.get() >= 0) {
                check_spawn(posix_spawn_file_actions_adddup2(&setup.actions, input.child.get(), 0),
                            "map child stdin");
            } else {
                check_spawn(
                    posix_spawn_file_actions_addopen(&setup.actions, 0, "/dev/null", O_RDONLY, 0),
                    "open child stdin");
            }
            check_spawn(posix_spawn_file_actions_adddup2(&setup.actions, out.write_end.get(), 1),
                        "map child stdout");
            check_spawn(posix_spawn_file_actions_adddup2(&setup.actions, err.write_end.get(), 2),
                        "map child stderr");
            for (size_t i = 0; i < inherited_copies.size(); ++i) {
                check_spawn(
                    posix_spawn_file_actions_adddup2(&setup.actions, inherited_copies[i].get(),
                                                    3 + static_cast<int>(i)),
                    "map inherited descriptor");
            }
            check_spawn(posix_spawn_file_actions_addclosefrom_np(&setup.actions, first_unused),
                        "close unlisted descriptors");
            check_spawn(posix_spawnattr_setflags(&setup.attributes, POSIX_SPAWN_SETPGROUP),
                        "configure child process group");
            check_spawn(posix_spawnattr_setpgroup(&setup.attributes, 0), "set child process group");
            pid_t pid = -1;
            check_spawn(
                posix_spawn(&pid, argv[0], &setup.actions, &setup.attributes, argv.data(), env.data()),
                "spawn");
            return pid;
        }

        // Retain PID ownership during I/O; callbacks and system calls may throw.
        struct ChildCleanup {
            pid_t pid;
            bool reaped = false;

            ~ChildCleanup() {
                if (!reaped) {
                    kill(-pid, SIGKILL);
                    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                    }
                }
            }
            ChildCleanup(const ChildCleanup &) = delete;
            auto operator=(const ChildCleanup &) -> ChildCleanup & = delete;
        };
    } // namespace

    auto run_process(const std::vector<std::string> &args, const ProcessOptions &options)
        -> Result {
        Pipe out, err;
        InputStream input(!options.stdin_data.empty());
        const pid_t pid = spawn_process(args, options, out, err, input);
        ChildCleanup cleanup{pid};
        out.write_end.reset();
        err.write_end.reset();
        input.child.reset();

        Result result;
        const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
        size_t input_offset = 0;
        auto feed_input = [&] {
            for (int i = 0; input.parent.get() >= 0 && i < io_batch_operations; ++i) {
                const auto remaining = options.stdin_data.substr(input_offset);
                const auto count = send(input.parent.get(), remaining.data(),
                                        std::min(remaining.size(), stdin_chunk_bytes),
                                        MSG_DONTWAIT | MSG_NOSIGNAL);
                if (count > 0) {
                    input_offset += static_cast<size_t>(count);
                    if (input_offset == options.stdin_data.size()) {
                        input.parent.reset(); // Deliver EOF after the last byte.
                    }
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                } else if (count < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                    input.parent.reset(); // Child closed stdin; collect its result.
                } else if (count < 0) {
                    system_error("write child stdin");
                } else {
                    throw std::runtime_error("write child stdin returned zero");
                }
            }
        };
        auto drain = [&](UniqueFd &descriptor, std::string &target, bool stderr_stream) {
            std::array<char, output_chunk_bytes> buffer{};
            // Bound each batch so continuous output cannot starve timeout/cancel checks.
            for (int i = 0; descriptor.get() >= 0 && i < io_batch_operations; ++i) {
                const auto count = read(descriptor.get(), buffer.data(), buffer.size());
                if (count > 0) {
                    const auto room = options.output_limit - result.out.size() - result.err.size();
                    const auto retained = std::min(room, static_cast<size_t>(count));
                    target.append(buffer.data(), retained);
                    if (options.on_output && retained > 0) {
                        options.on_output(stderr_stream, {buffer.data(), retained});
                    }
                    if (static_cast<size_t>(count) > room) {
                        result.output_limited = true;
                    }
                } else if (count == 0) {
                    descriptor.reset();
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    system_error(stderr_stream ? "read child stderr" : "read child stdout");
                }
            }
        };

        for (;;) {
            drain(out.read_end, result.out, false);
            drain(err.read_end, result.err, true);
            feed_input();
            // Observe exit without reaping so group cleanup cannot target a reused PID.
            siginfo_t info{};
            const int observed = waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if (observed < 0 && errno != EINTR) {
                system_error("observe child exit");
            }
            if (options.cancel && options.cancel->load()) {
                result.cancelled = true;
            }
            if (result.cancelled || result.output_limited) {
                break;
            }
            if (observed == 0 && info.si_pid &&
                (!options.drain_until_eof || (out.read_end.get() < 0 && err.read_end.get() < 0))) {
                drain(out.read_end, result.out, false);
                drain(err.read_end, result.err, true);
                break;
            }
            if (Clock::now() >= deadline) {
                result.timed_out = true;
                break;
            }
            pollfd fds[] = {{out.read_end.get(), POLLIN, 0},
                            {err.read_end.get(), POLLIN, 0},
                            {input.parent.get(), POLLOUT, 0}};
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                    .count();
            const auto wait_ms =
                static_cast<int>(std::clamp<int64_t>(remaining_ms, 0, cancellation_poll_ms));
            if (poll(fds, 3, wait_ms) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                system_error("poll child streams");
            }
            for (const auto &fd : fds) {
                if (fd.revents & POLLNVAL) {
                    throw std::runtime_error("poll child streams: invalid descriptor");
                }
            }
        }

        // This group covers the launched command; container/Guest cleanup belongs to its caller.
        if (result.timed_out || result.output_limited || result.cancelled ||
            options.kill_remaining_group) {
            kill(-pid, SIGKILL);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                system_error("reap child");
            }
        }
        cleanup.reaped = true;
        if (WIFEXITED(status)) {
            result.runtime_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.runtime_status = 128 + WTERMSIG(status);
        }
        return result;
    }

    void check_process_result(const Result &result) {
        require(result.runtime_status == 0 && !result.timed_out && !result.output_limited &&
                    !result.cancelled,
                "process failed: " + result.err);
    }
} // namespace lib
