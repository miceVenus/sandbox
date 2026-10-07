# bbm-sandbox

面向 Linux 的 C++17 Sandbox SDK，为 AI Agent 提供独立 Git 工作区、命令执行、文件读写和改动预览。支持 libcrun 容器与 libkrun microVM。

## 构建

需要 CMake 3.21+、C/C++ 编译器、Rust/Cargo 和 vcpkg。Ubuntu/Debian 构建依赖：

```sh
sudo apt-get install build-essential git cmake python3 autoconf automake libtool pkg-config \
  libsystemd-dev libseccomp-dev libcap-dev libjson-c-dev \
  clang llvm libclang-dev bison flex libssl-dev libelf-dev bc xz-utils patch patchelf
```

将已 bootstrap 的 vcpkg 放在项目旁的 `../vcpkg`，CMake 会自动使用它，通过 manifest 安装 libgit2 和 nlohmann_json。其他位置可在配置时添加 `-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`。

首次构建运行时依赖，安装到项目的 `.deps/prefix`：

```sh
./tools/build-deps.sh --jobs 4
```

Debug：

```sh
cmake --preset debug
cmake --build --preset debug
```

Release，安装到 `build/install`：

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release
```

Test：

```sh
cmake --preset test
cmake --build --preset test
ctest --preset test
```

三个预设默认启用 microVM。仅构建容器版本时，依赖脚本添加 `--without-krun`，CMake 配置添加 `-DSANDBOX_ENABLE_LIBKRUN=OFF`。

## 运行样例

普通用户运行。容器需要 user namespace、cgroup v2 的 memory/pids 委派和 systemd 用户会话；microVM 还需要 `/dev/kvm` 访问权限。HostTools 样例使用本机 `/usr/bin/git` 和 `/usr/bin/g++`；Minimal 测试需静态 BusyBox。

```sh
cmake --preset debug
cmake --build build/debug --target sandbox-example -j4

./build/debug/examples/sandbox-example container
./build/debug/examples/sandbox-example microvm
```

样例自动创建临时 Git 仓库，演示读写、编译运行、流式输出、diff 和清理。样例显式关闭 CPU 配额，内存预算为 512 MiB。

源码见 [sdk_example.cpp](examples/sdk_example.cpp)，独立应用的链接方法见 [样例说明](examples/README.md)。
