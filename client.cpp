#include "client.hpp"

#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

Result CrunClient::call(const std::vector<std::string> &options,
                        int timeout_ms,
                        bool drain_until_eof,
                        size_t output_limit,
                        std::string_view stdin_data) {
    std::vector<std::string> args{binary_};
    if (systemd_cgroups_) {
        // Only host runtime connection settings are passed to crun. OCI process.env
        // remains separate and never receives the user's D-Bus connection.
        const char *configured = std::getenv("XDG_RUNTIME_DIR");
        auto runtime = configured ? std::filesystem::path(configured)
                                  : std::filesystem::path("/run/user") / std::to_string(geteuid());
        struct stat st{};
        if (!runtime.is_absolute() || lstat(runtime.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 0077)) {
            throw std::runtime_error("rootless crun requires an owned, private XDG_RUNTIME_DIR and "
                                     "a systemd user session");
        }
        const auto *account = getpwuid(geteuid());
        if (!account) {
            throw std::runtime_error("cannot determine runtime user home");
        }
        args = {"/usr/bin/env",
                "XDG_RUNTIME_DIR=" + runtime.string(),
                "DBUS_SESSION_BUS_ADDRESS=unix:path=" + (runtime / "bus").string(),
                "HOME=" + std::string(account->pw_dir),
                binary_,
                "--systemd-cgroup"};
    }
    if (!root_.empty()) {
        args.push_back("--root");
        args.push_back(root_.string());
    }
    args.insert(args.end(), options.begin(), options.end());
    return run_process(args, timeout_ms, output_limit, drain_until_eof, stdin_data);
}

Result CrunClient::start(const std::string &id, const std::string &bundle) {
    // Detached PID 1 may retain the pipes; do not wait for its lifetime.
    return call({"run", "--detach", "--bundle", bundle, id}, 10000, false);
}

Result CrunClient::exec(const std::string &id,
                        const std::string &cwd,
                        const std::vector<std::string> &argv,
                        int ms,
                        size_t output_limit,
                        std::string_view stdin_data) {
    std::vector<std::string> args{"exec", "--no-new-privs", "--cwd", cwd, id};
    args.insert(args.end(), argv.begin(), argv.end());
    return call(args, ms, true, output_limit, stdin_data);
}

Result CrunClient::state(const std::string &id) {
    std::vector<std::string> args{"state", id};
    return call(args, 10000, true);
}

bool CrunClient::destroy(const std::string &id) {
    const auto killed = call({"kill", "--all", id, "KILL"});
    const auto deleted = call({"delete", "--force", id});
    const bool ok = deleted.runtime_status == 0 && !deleted.timed_out && !deleted.output_limited;
    if (!ok) {
        std::cerr << "cleanup failed; inspect container manually:\n" << killed.err << deleted.err;
    }
    return ok;
}
Result CrunClient::pause(const std::string &id) {
    return call({"pause", id});
}
Result CrunClient::resume(const std::string &id) {
    return call({"resume", id});
}
