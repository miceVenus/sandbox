#include "../include/sandbox.hpp"
#include "../include/virtualization/host_tools.hpp"
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <unistd.h>
using json = nlohmann::json;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void write(const fs::path &p, const std::string &text) {
    std::ofstream(p) << text;
}
template <class F> void rejects(F fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "expected rejection");
}
// Test double for manager policies; deliberately provides no isolation.
class FakeRuntime final : public RuntimeBackend {
  public:
    std::string id() const override {
        return "oci-crun";
    }
    void configure_state_directory(const fs::path &directory) override {
        root_ = directory;
        fs::create_directories(root_);
    }
    void validate_options(const Options &) override {
    }
    void prepare(SandboxInfo &) override {
    }
    void start(SandboxInfo &info) override {
        ::write(root_ / info.runtime_id, "running");
    }
    RuntimeStatus status(const SandboxInfo &info) override {
        if (!fs::exists(root_ / info.runtime_id)) {
            return {RuntimeState::Missing, true, "missing", {}};
        }
        std::string value;
        std::ifstream(root_ / info.runtime_id) >> value;
        const auto kind = value == "running"  ? RuntimeState::Running
                          : value == "paused" ? RuntimeState::Paused
                                              : RuntimeState::Stopped;
        return {kind, true, value, {}};
    }
    Result execute(const SandboxInfo &info, const RuntimeCommand &command) override {
        check(command.cwd == "/project/src", "unexpected runtime cwd");
        const auto &action = command.argv.at(0);
        if (action == "/bin/write") {
            ::write(info.work_files_dir / "src/a", "changed\n");
            ::write(info.work_files_dir / "new", "new\n");
            return {0, false, false, "done\n", {}};
        }
        if (action == "/bin/fail") {
            return {7, false, false, {}, {}};
        }
        if (action == "/bin/flood") {
            return run_process({"/bin/sh", "-c", "while :; do echo x; done"},
                               command.timeout_ms,
                               command.output_limit);
        }
        if (action == "/bin/sleep") {
            return run_process({"/bin/sleep", "5"}, command.timeout_ms, command.output_limit);
        }
        throw std::runtime_error("unknown test command");
    }
    Result read(const SandboxInfo &info, const fs::path &path, size_t limit) override {
        std::ifstream in(info.work_files_dir / path.lexically_relative("/project"),
                         std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)), {});
        check(content.size() <= limit, "fixture exceeds read limit");
        return {0, false, false, content, {}};
    }
    Result write(const SandboxInfo &info, const fs::path &path, std::string_view content) override {
        ::write(info.work_files_dir / path.lexically_relative("/project"), std::string(content));
        return {0, false, false, {}, {}};
    }
    void pause(const SandboxInfo &info) override {
        ::write(root_ / info.runtime_id, "paused");
    }
    void resume(const SandboxInfo &info) override {
        ::write(root_ / info.runtime_id, "running");
    }
    void synchronize_workspace(const SandboxInfo &) override {
    }
    void stop(const SandboxInfo &info) override {
        fs::remove(root_ / info.runtime_id);
    }

  private:
    fs::path root_;
};
int main() {
    char pattern[] = "/tmp/sandbox-manager-test-XXXXXX";
    fs::path temp = mkdtemp(pattern);
    try {
        auto repo = temp / "source", root = temp / "manager";
        fs::create_directory(repo);
        auto git = [&](std::vector<std::string> args) {
            args.insert(args.begin(), {"/usr/bin/git", "-C", repo.string()});
            auto r = run_process(args, 10000);
            check(r.runtime_status == 0, "fixture Git failed");
        };
        git({"init", "-q"});
        fs::create_directory(repo / "src");
        write(repo / "src/a", "original\n");
        git({"add", "."});
        git({"-c", "user.name=Test", "-c", "user.email=test@example.org", "commit", "-qm", "base"});
        fs::create_directory(root);
        fs::permissions(root, fs::perms::owner_all);
        const std::string id(32, 'a'), cid = "bbm-sandbox-" + id;
        fs::create_directory(root / id);
        auto w = GitWorkspace::create(repo, root / id / "workspace");
        Sandbox manager(root, std::make_unique<FakeRuntime>());
        // Policy validation must reject replacing tool/data mount targets before startup.
        Options invalid;
        invalid.src_repo = repo;
        invalid.environment = static_cast<Environment>(100);
        rejects([&] { manager.create(invalid); });
        invalid.environment = Environment::HostTools;
        for (const auto &path :
             {"/usr/bin/work", "/build", "/env/python", "/cache", "/sandbox-tools"}) {
            invalid.ctr_repo = path;
            rejects([&] { make_libcrun_backend()->validate_options(invalid); });
        }
        // Trusted A and private manager metadata cannot lie in imported host tool directories.
        validate_host_tools(repo, root);
        rejects([&] { validate_host_tools("/usr/share/private-repo", root); });
        rejects([&] { validate_host_tools(repo, "/usr/lib/private-manager"); });
        rejects([&] { validate_host_tools("/usr", root); });
        auto fixture = [&] {
            json record = {{"id", id},
                           {"container_id", cid},
                           {"state", int(SandboxState::Active)},
                           {"source", repo.string()},
                           {"revision", "HEAD"},
                           {"workspace", "/project"},
                           {"cwd", "src"},
                           {"memory", 268435456},
                           {"period", 100000},
                           {"quota", 100000},
                           {"pids", 64},
                           {"timeout", 100},
                           {"output", 1024},
                           {"policy", 0},
                           {"baseline", w.baseline()},
                           {"source_head", w.baseline()},
                           {"branch", nullptr},
                           {"error", ""}};
            write(root / id / "sandbox.json", record.dump());
            write(root / "runtime" / cid, "running");
        };
        fixture();
        fs::rename(root / id / "sandbox.json", root / id / "session.json");
        rejects([&] { manager.read("/project/src/a"); });
        manager.open(id);
        rejects([&] { manager.open(id); });
        rejects([&] { manager.create(invalid); });
        const auto legacy = manager.status().info;
        check(legacy.options.environment == Environment::Minimal,
              "legacy info profile was not preserved");
        check(manager.status().runtime_status == "running", "state query failed");
        check(manager.read("/project/src/a") == "original\n", "SDK read failed");
        const std::string binary("a\0\xff", 3);
        manager.write("/project/src/sdk-file", binary);
        check(manager.read("/project/src/sdk-file") == binary, "SDK binary roundtrip failed");
        manager.write("/project/src/sdk-file", {});
        check(manager.read("/project/src/sdk-file").empty(), "empty write failed");
        rejects([&] { manager.read("src/a"); });
        rejects([&] { manager.write("src/a", "bad"); });
        rejects(
            [&] { manager.write("/project/src/a", std::string(8 * 1024 * 1024 + 1, 'x')); });
        fs::remove(w.files_path() / "src/sdk-file");
        rejects([&] { manager.execute(CommandRequest{{"/bin/write"}, "../outside"}); });
        rejects([&] { manager.execute(std::vector<std::string>{"relative"}); });
        rejects([&] { Sandbox other(root, std::make_unique<FakeRuntime>()); other.open("bad"); });
        rejects(
            [&] { Sandbox other(root, std::make_unique<FakeRuntime>()); other.open(std::string(32, 'b')); });
        rejects([&] { manager.execute(CommandRequest{{"/bin/write"}, "/tmp"}); });
        rejects([&] {
            manager.execute(CommandRequest{{"/bin/write"}, fs::path(std::string("a\0b", 3))});
        });
        int lock = open((root / id / "lock").c_str(), O_RDWR);
        check(flock(lock, LOCK_EX | LOCK_NB) == 0, "test lock failed");
        rejects([&] { manager.get_changes(); });
        close(lock);
        auto r = manager.execute(std::vector<std::string>{"/bin/write"});
        check(r.out == "done\n", "exec output wrong");
        check(manager.execute(std::vector<std::string>{"/bin/fail"}).runtime_status == 7,
              "exit status lost");
        check(manager.get_status() == SandboxState::Active, "nonzero task exit closed info");
        auto changes = manager.get_changes();
        check(changes.diff.find("+changed") != std::string::npos &&
                  changes.diff.find("+new") != std::string::npos,
              "diff missing changes");
        check(manager.status().runtime_status == "running",
              "preview did not resume");
        std::string original;
        std::ifstream(repo / "src/a") >> original;
        check(original == "original", "source changed");
        check(manager.execute(std::vector<std::string>{"/bin/flood"}).output_limited,
              "output limit missing");
        check(manager.get_status() == SandboxState::Failed, "output failure not persisted");
        check(!manager.get_changes().diff.empty(), "failed info lost changes");
        rejects([&] { manager.execute(std::vector<std::string>{"/bin/write"}); });
        rejects([&] { manager.read("/project/src/a"); });
        rejects([&] { manager.write("/project/src/a", "bad"); });
        fixture();
        check(manager.execute(std::vector<std::string>{"/bin/sleep"}).timed_out,
              "timeout missing");
        check(manager.get_status() == SandboxState::Failed, "timeout state wrong");
        fixture();
        manager.stop();
        check(manager.get_status() == SandboxState::Stopped, "stop state wrong");
        rejects([&] { manager.execute(std::vector<std::string>{"/bin/write"}); });
        fixture();
        write(root / "runtime" / cid, "stopped");
        rejects([&] { manager.execute(std::vector<std::string>{"/bin/write"}); });
        check(manager.get_status() == SandboxState::Failed, "dead container state not persisted");
        // Each handle owns exactly one independent B, even in shared storage.
        Options options;
        options.src_repo = repo;
        options.ctr_repo = "/project";
        options.cwd_rlt = "src";
        Sandbox first(root, std::make_unique<FakeRuntime>());
        Sandbox second(root, std::make_unique<FakeRuntime>());
        const auto one = first.create(options);
        const auto two = second.create(options);
        check(one.id != two.id && one.directory != two.directory,
              "separate Sandbox objects share identity or workspace");
        first.write("/project/src/a", "first only\n");
        check(second.read("/project/src/a") == "original\n",
              "Sandbox write affected another object");
        rejects([&] { first.create(options); });
        first.destroy();
        first.destroy();
        check(first.get_status() == SandboxState::Discarded, "discarded state was lost");
        rejects([&] { first.read("/project/src/a"); });
        check(second.read("/project/src/a") == "original\n",
              "destroy reclaimed another Sandbox");
        second.destroy();
        fs::remove_all(temp);
        std::cout << "manager tests passed\n";
    } catch (const std::exception &e) {
        fs::remove_all(temp);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
