#include "process.hpp"
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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Pipe {
    int read_fd = -1, write_fd = -1;
    Pipe() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            throw std::runtime_error("pipe2 failed");
        }
        read_fd = fds[0];
        write_fd = fds[1];
        if (fcntl(read_fd, F_SETFL, O_NONBLOCK) < 0) {
            close(read_fd);
            close(write_fd);
            throw std::runtime_error("fcntl failed");
        }
    }
    void close_read() {
        if (read_fd >= 0) {
            close(read_fd);
        }
        read_fd = -1;
    }
    void close_write() {
        if (write_fd >= 0) {
            close(write_fd);
        }
        write_fd = -1;
    }
    ~Pipe() {
        close_read();
        close_write();
    }
};

// A socket pair gives the child stdin and lets the manager send with
// MSG_NOSIGNAL, without changing the application's global SIGPIPE handler.
struct InputStream {
    int child_fd = -1;
    int parent_fd = -1;

    explicit InputStream(bool enabled) {
        if (!enabled) {
            return;
        }
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
            throw std::runtime_error("stdin socketpair failed");
        }
        child_fd = fds[0];
        parent_fd = fds[1];
    }
    void close_child() {
        if (child_fd >= 0) {
            close(child_fd);
            child_fd = -1;
        }
    }
    void close_parent() {
        if (parent_fd >= 0) {
            close(parent_fd);
            parent_fd = -1;
        }
    }
    ~InputStream() {
        close_child();
        close_parent();
    }
};

// Read both streams without blocking; cap their combined retained size.
Result run_process(const std::vector<std::string> &args,
                   int timeout_ms,
                   size_t output_limit,
                   bool drain_until_eof,
                   std::string_view stdin_data) {
    if (args.empty()) {
        throw std::runtime_error("empty argv");
    }
    Pipe out, err;
    InputStream input(!stdin_data.empty());
    std::vector<char *> argv;
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    // The runtime gets a small, fixed environment instead of host secrets.
    char path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    char locale[] = "LANG=C";
    char *env[] = {path, locale, nullptr};

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) {
        throw std::runtime_error(std::strerror(rc));
    }
    rc = posix_spawnattr_init(&attrs);
    if (rc) {
        posix_spawn_file_actions_destroy(&actions);
        throw std::runtime_error(std::strerror(rc));
    }

    auto check = [&](int value) {
        if (!rc && value) {
            rc = value;
        }
    };

    if (input.child_fd >= 0) {
        check(posix_spawn_file_actions_adddup2(&actions, input.child_fd, 0));
    } else {
        check(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0));
    }
    check(posix_spawn_file_actions_adddup2(&actions, out.write_fd, 1));
    check(posix_spawn_file_actions_adddup2(&actions, err.write_fd, 2));
    // GNU/glibc extension: do not leak the manager's descriptors into crun.
    check(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
    check(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP));
    check(posix_spawnattr_setpgroup(&attrs, 0));
    pid_t pid = -1;
    if (!rc) {
        rc = posix_spawn(&pid, argv[0], &actions, &attrs, argv.data(), env);
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    if (rc) {
        throw std::runtime_error("spawn: " + std::string(std::strerror(rc)));
    }
    out.close_write();
    err.close_write();
    input.close_child();

    Result result;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t input_offset = 0;

    auto feed_input = [&] {
        for (int i = 0; input.parent_fd >= 0 && i < 16; ++i) {
            const auto remaining = stdin_data.substr(input_offset);
            const ssize_t n = send(input.parent_fd,
                                   remaining.data(),
                                   std::min<size_t>(remaining.size(), 16384),
                                   MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                input_offset += static_cast<size_t>(n);
                if (input_offset == stdin_data.size()) {
                    input.close_parent(); // EOF after all bytes are delivered.
                }
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                input.close_parent(); // Child closed stdin; collect its result.
            }
        }
    };

    auto drain = [&](Pipe &pipe, std::string &target) {
        std::array<char, 4096> buf{};
        // Bounded reads keep a continuously writing task from starving the timer.
        for (int i = 0; pipe.read_fd >= 0 && i < 16; ++i) {
            const ssize_t n = read(pipe.read_fd, buf.data(), buf.size());
            if (n > 0) {
                const size_t room = output_limit - result.out.size() - result.err.size();
                target.append(buf.data(), std::min(room, static_cast<size_t>(n)));
                if (static_cast<size_t>(n) > room) {
                    result.output_limited = true;
                }
            } else if (n == 0) {
                pipe.close_read();
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                pipe.close_read();
                break;
            }
        }
    };

    int status = 0;
    for (;;) {
        drain(out, result.out);
        drain(err, result.err);
        feed_input();
        // Observe exit without reaping: retain PID ownership until pipe cleanup.
        siginfo_t info{};
        const int observed = waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
        if (observed < 0 && errno != EINTR) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("waitid failed");
        }
        if (result.output_limited) {
            break;
        }
        if (observed == 0 && info.si_pid &&
            (!drain_until_eof || (out.read_fd < 0 && err.read_fd < 0))) {
            drain(out, result.out);
            drain(err, result.err);
            break;
        }
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        pollfd fds[] = {
            {out.read_fd, POLLIN, 0}, {err.read_fd, POLLIN, 0}, {input.parent_fd, POLLOUT, 0}};
        poll(fds, 3, 10);
    }
    // This kills only the runtime client group. The caller must also destroy
    // the container on cancellation: its processes need not share this group.
    if (result.timed_out || result.output_limited) {
        kill(-pid, SIGKILL);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("waitpid failed");
        }
    }
    if (WIFEXITED(status)) {
        result.runtime_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.runtime_status = 128 + WTERMSIG(status);
    }
    return result;
}
