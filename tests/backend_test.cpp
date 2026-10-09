#include "lib/elf.hpp"
#include "lib/process.hpp"
#include "sandbox.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    void check(bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    // Test double for a guest transport. It provides no actual isolation.
    // Its guest files are intentionally separate from host B until synchronization.
    class GuestBackend final : public RuntimeBackend {
      public:
        auto id() const -> std::string override {
            return "test-guest";
        }
        void configure_state_directory(const fs::path &) override {
        }
        void validate_options(const Options &) override {
        }
        void prepare(SandboxInfo &info) override {
            info_ = info;
            std::ifstream source(info.work_files_dir / "a");
            std::getline(source, guests_[info.id].content);
            guests_[info.id].content += '\n';
        }
        void start(SandboxInfo &info) override {
            guests_[info.id].state = RuntimeState::Running;
            info.resource_limits_verified = true;
        }
        auto status(const SandboxInfo &info) -> RuntimeStatus override {
            const auto state = guests_.at(info.id).state;
            return {state, true, state == RuntimeState::Running ? "running" : "missing", {}};
        }
        auto execute(const RuntimeCommand &command) -> Result override {
            const auto &info = info_;
            check(command.cwd.lexically_relative("/workspace") == ".",
                  "manager passed an unexpected guest cwd");
            auto &guest = guests_.at(info.id);
            guest.content = "guest change\n";
            if (command.argv[0] == "/guest/timeout") {
                return {-1, true, false, {}, {}};
            }
            return {0, false, false, "executed in guest\n", {}};
        }
        auto read(const fs::path &path, size_t limit) -> Result override {
            const auto &info = info_;
            check(path == "/workspace/a", "unexpected guest read path");
            const auto &content = guests_.at(info.id).content;
            check(content.size() <= limit, "guest read limit exceeded");
            return {0, false, false, content, {}};
        }
        auto write(const fs::path &path, std::string_view content) -> Result override {
            const auto &info = info_;
            check(path == "/workspace/a", "unexpected guest write path");
            guests_.at(info.id).content = std::string(content);
            return {0, false, false, {}, {}};
        }
        void pause(const SandboxInfo &info) override {
            guests_.at(info.id).state = RuntimeState::Paused;
        }
        void resume(const SandboxInfo &info) override {
            guests_.at(info.id).state = RuntimeState::Running;
        }
        void synchronize_workspace(const SandboxInfo &info) override {
            const auto &guest = guests_.at(info.id);
            check(guest.state != RuntimeState::Running,
                  "workspace exported while guest was writing");
            std::ofstream(info.work_files_dir / "a", std::ios::binary) << guest.content;
        }
        void stop(const SandboxInfo &info) override {
            guests_.at(info.id).state = RuntimeState::Missing;
            synchronize_workspace(info);
        }

      private:
        SandboxInfo info_;
        struct Guest {
            RuntimeState state = RuntimeState::Missing;
            std::string content;
        };
        std::map<std::string, Guest> guests_;
    };
} // namespace

auto main() -> int {
    char pattern[] = "/tmp/sandbox-backend-test-XXXXXX";
    const auto *created = mkdtemp(pattern);
    if (!created) {
        return 1;
    }
    const fs::path temporary = created;
    try {
        const auto dependencies = lib::dynamic_dependency_paths(
            "\tlinux-vdso.so.1 (0x000001)\n"
            "\tlibkrun.so.1 => /tmp/sdk install/libexec/bbm-sandbox/lib/libkrun.so.1 (0x000002)\n"
            "\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x000003)\n"
            "\t/lib64/ld-linux-x86-64.so.2 (0x000004)\n");
        check(dependencies ==
                  std::vector<fs::path>{"/tmp/sdk install/libexec/bbm-sandbox/lib/libkrun.so.1",
                                        "/lib/x86_64-linux-gnu/libc.so.6",
                                        "/lib64/ld-linux-x86-64.so.2"},
              "runtime library paths containing spaces were truncated");
        bool missing_rejected = false;
        try {
            lib::dynamic_dependency_paths("\tlibkrun.so.1 => not found\n");
        } catch (const std::exception &) {
            missing_rejected = true;
        }
        check(missing_rejected, "unresolved runtime library was silently ignored");
        const auto source = temporary / "source";
        fs::create_directory(source);
        std::ofstream(source / "a") << "original\n";
        auto git = [&](const std::vector<std::string> &arguments) {
            std::vector<std::string> command{"/usr/bin/git", "-C", source.string()};
            command.insert(command.end(), arguments.begin(), arguments.end());
            check(lib::run_process(command).runtime_status == 0,
                  "fixture Git operation failed");
        };
        git({"init", "-q"});
        git({"add", "."});
        git({"-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm",
             "base"});
        const auto root = temporary / "manager";
        Sandbox manager(root, std::make_unique<GuestBackend>());
        Options options;
        options.src_repo = source;
        // This backend needs neither CPU delegation nor an OCI bundle/native PID.
        const auto info = manager.create(options);
        bool duplicate_rejected = false;
        try {
            manager.create(options);
        } catch (const std::exception &) {
            duplicate_rejected = true;
        }
        check(duplicate_rejected, "one Sandbox created multiple runtimes");
        check(info.runtime_backend == "test-guest", "backend identity was not recorded");
        check(!fs::exists(info.bundle_dir), "manager prepared OCI artifacts for another backend");
        check(manager.read("/workspace/a") == "original\n", "guest read failed");
        manager.write("/workspace/a", "guest API write\n");
        check(manager.read("/workspace/a") == "guest API write\n", "guest write failed");
        check(manager.execute(std::vector<std::string>{"/guest/modify"}).out ==
                  "executed in guest\n",
              "guest exec failed");
        std::string host_copy;
        std::ifstream(info.work_files_dir / "a") >> host_copy;
        check(host_copy == "original", "guest transport bypassed synchronization");
        check(manager.get_changes().diff.find("+guest change") != std::string::npos,
              "manager inspected stale host files instead of guest results");
        check(manager.get_status() == SandboxState::Active, "preview did not resume guest");
        Sandbox wrong_backend(root);
        bool refused = false;
        try {
            wrong_backend.open(info.id);
        } catch (const std::exception &error) {
            refused =
                std::string(error.what()).find("different runtime backend") != std::string::npos;
        }
        check(refused, "info was opened with an incompatible backend");
        check(manager.execute(std::vector<std::string>{"/guest/timeout"}).timed_out,
              "timeout result lost");
        check(manager.get_status() == SandboxState::Failed, "timeout did not close guest");
        check(manager.get_changes().diff.find("+guest change") != std::string::npos,
              "timeout lost guest changes");
        manager.stop();
        manager.stop();
        check(manager.get_status() == SandboxState::Stopped, "repeated guest stop failed");
        manager.destroy();
        manager.destroy();
        check(manager.get_status() == SandboxState::Discarded && !fs::exists(info.directory),
              "destroy did not discard the bound Sandbox");
        std::ifstream(source / "a") >> host_copy;
        check(host_copy == "original", "source repository was changed");
        fs::remove_all(temporary);
        std::cout << "backend substitution test passed\n";
    } catch (const std::exception &error) {
        fs::remove_all(temporary);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
