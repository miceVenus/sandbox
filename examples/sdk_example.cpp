#include "lib/process.hpp"
#include "sandbox.hpp"
#include "virtualization/microvm/krun_runtime.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    constexpr const char *original_code = R"cpp(#include <iostream>

int main() {
    std::cout << "Hello from the source workspace!\n";
}
)cpp";

    constexpr const char *modified_code = R"cpp(#include <iostream>

int main() {
    std::cout << "Hello from the sandbox!\n";
}
)cpp";

    void require_success(const Result &result, const std::string &operation) {
        if (result.runtime_status != 0 || result.timed_out || result.output_limited ||
            result.cancelled) {
            throw std::runtime_error(operation + ": exit=" + std::to_string(result.runtime_status) +
                                     ", timeout=" + std::to_string(result.timed_out) +
                                     ", output_limit=" + std::to_string(result.output_limited) +
                                     ", cancelled=" + std::to_string(result.cancelled) + "\n" +
                                     result.err);
        }
    }

    // Git CLI only prepares the demo input; sandbox workspace management uses libgit2.
    void prepare_source(const fs::path &source) {
        fs::create_directory(source);
        std::ofstream output(source / "main.cpp");
        output << original_code;
        output.close();
        if (!output) {
            throw std::runtime_error("无法创建样例源文件");
        }
        auto git = [&](std::vector<std::string> arguments) {
            arguments.insert(arguments.begin(),
                             {"/usr/bin/git", "-C", source.string(), "-c",
                              "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false"});
            require_success(lib::run_process(arguments), "准备临时 Git 仓库");
        };
        git({"init", "-q"});
        git({"add", "main.cpp"});
        git({"-c", "user.name=SDK Example", "-c", "user.email=example@localhost", "commit", "-qm",
             "Example baseline"});
    }
} // namespace

auto main(int argc, char **argv) -> int {
    const std::string mode = argc > 1 ? argv[1] : "container";
    if (argc > 2 || (mode != "container" && mode != "microvm")) {
        std::cerr << "用法: " << argv[0] << " [container|microvm]\n";
        return 2;
    }

    fs::path demo_directory;
    std::unique_ptr<Sandbox> sandbox;
    bool created = false;
    try {
        auto runtime = mode == "microvm" ? make_krun_backend() : make_crun_backend();
        // A short, exclusively owned directory also keeps Unix socket paths within their limit.
        char directory_template[] = "/tmp/sbx-example-XXXXXX";
        const auto *directory = mkdtemp(directory_template);
        if (!directory) {
            throw std::runtime_error("无法创建临时目录");
        }
        demo_directory = directory;
        const auto source = demo_directory / "source";
        prepare_source(source);

        // One Sandbox object owns one environment; the backend selects container or microVM.
        sandbox = std::make_unique<Sandbox>(demo_directory / "state", std::move(runtime));
        Options options;
        options.src_repo = source;
        options.ctr_repo = "/workspace";
        options.environment = Environment::HostTools;
        options.memory_bytes = 512 * 1024 * 1024;
        options.cmd_timeout = std::chrono::seconds(10);
        options.policy = MergePolicy::ReviewOnly;
        // Explicit demo policy: this machine does not delegate the CPU controller.
        options.cpu_quota_us = 0;

        std::cout << "[create] 模式: " << mode << "，CPU 配额关闭，内存上限 512 MiB\n"
                  << "宿主临时源仓库: " << source << std::endl;
        const auto info = sandbox->create(options);
        created = true;
        std::cout << "Sandbox ID: " << info.id << "\n"
                  << "运行状态: " << sandbox->status().runtime_status << "\n";

        std::cout << "\n[read] 沙箱中的原始 main.cpp:\n" << sandbox->read("/workspace/main.cpp");
        sandbox->write("/workspace/main.cpp", modified_code);
        std::cout << "\n[write] 已修改沙箱中的 main.cpp\n";

        auto on_output = [](OutputStream stream, std::string_view bytes) {
            auto &output = stream == OutputStream::Stdout ? std::cout : std::cerr;
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
        };
        std::cout << "\n[execute] 在沙箱内编译 C++ 程序\n";
        require_success(sandbox->execute({"/usr/bin/g++", "-std=c++17", "/workspace/main.cpp", "-o",
                                          "/build/sdk-example"},
                                         on_output),
                        "编译");
        std::cout << "[execute] 运行编译产物，输出通过回调实时显示:\n";
        require_success(sandbox->execute({"/build/sdk-example"}, on_output), "运行");

        // CommandRequest demonstrates relative cwd and stdin without shell concatenation.
        CommandRequest request;
        request.argv = {"/bin/cat"};
        request.cwd_relative = ".";
        request.stdin_data = "SDK stdin -> sandbox stdout\n";
        require_success(sandbox->execute(request, on_output), "标准输入演示");

        const auto changes = sandbox->get_changes();
        std::cout << "\n[get_changes] 相对起始 commit 的修改:\n" << changes.status << changes.diff;
        std::ifstream input(source / "main.cpp");
        const std::string host_code{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
        if (!input || host_code != original_code) {
            throw std::runtime_error("宿主源文件验证失败");
        }
        std::cout << "\n宿主源文件未改变；修改仍在 B 工作区中。\n";

        sandbox->stop(); // Stop execution but keep the workspace and record.
        std::cout << "[stop] 已停止运行，B 工作区仍存在: " << fs::exists(info.work_files_dir)
                  << "\n";

        std::cout << "工作区将继续存在120s: " << demo_directory << "\n";
        sleep(120);

        sandbox->destroy(); // Explicitly discard B after runtime reclamation.
        created = false;
        fs::remove_all(demo_directory);
        std::cout << "[destroy] 沙箱与本次样例目录已清理。\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "样例失败: " << error.what() << '\n';
        if (created) {
            try {
                sandbox->destroy();
                std::cerr << "已回收沙箱运行资源和 B 工作区。\n";
            } catch (const std::exception &cleanup_error) {
                std::cerr << "回收失败: " << cleanup_error.what() << '\n';
            }
        }
        // Retain diagnostics on failure, including an unbound create() failure.
        if (!demo_directory.empty()) {
            std::cerr << "保留临时目录供排查: " << demo_directory << '\n';
        }
        return 1;
    }
}
