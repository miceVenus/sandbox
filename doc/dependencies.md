# 可移植依赖与验证流程

运行时依赖不再从某台机器的源码目录或 `$HOME/.local/opt/libkrun` 自动探测。
默认使用项目内 `.deps/prefix`。`deps.lock.json` 锁定 crun/libocispec、libkrun、
libkrunfw 的源码 commit，以及固件内核归档的 SHA-256。它锁定源码与构建选项，
不承诺不同编译器、发行版之间产生逐字节相同的二进制。

## 准备构建工具

当前依赖脚本支持 Linux 本机构建（x86_64、aarch64）。Ubuntu/Debian 可准备：

```bash
sudo apt-get update
sudo apt-get install build-essential git cmake python3 autoconf automake libtool pkg-config \
  libsystemd-dev libseccomp-dev libcap-dev libjson-c-dev \
  clang llvm libclang-dev bison flex libssl-dev libelf-dev bc xz-utils patch patchelf
```

另需已安装、满足锁定 libkrun 源码要求的 Rust/Cargo 工具链。脚本不会安装系统软件，
不需要 sudo。若 libclang 位于非标准位置，可由调用者设置 `LIBCLANG_PATH`；
`LLVM_CONFIG_PATH` 同样保持在构建环境中。libgit2/nlohmann_json 仍通过 vcpkg manifest
准备，项目旁边的 vcpkg toolchain 查找方式保持不变。

## 首次准备依赖

在项目根目录执行：

```bash
./tools/build-deps.sh --jobs 4
cmake --preset debug
cmake --build --preset debug
```

脚本在 `.deps/source` 下载固定 commit（包括固定的 Git submodule），在 `.deps/build`
构建 crun，在 `.deps/downloads` 缓存经过校验的内核归档；libkrun/固件使用各自上游的
源码树构建方式。所有开发文件安装到 `.deps/prefix`。

crun 使用静态库，并同时整理 `include/libcrun`、生成的 `include/ocispec` 和
`include/bbm-sandbox-deps/crun/config.h`。`config.h` 仅通过 libcrun imported target 的
私有使用者引入，不进入 SDK 公共头文件。libkrun 使用上游安装规则，统一安装到 `lib`；
Cargo 使用 `--locked`。完成后保存带构建身份和文件校验值的 manifest，重复执行会跳过
匹配的已完成产物。更换源码版本、编译器或构建选项需要重新准备。

脚本在编译 libcrun 前先运行 libocispec 的 `generate`，再生成 crun 的版本头文件，
避免直接构建 `libcrun.la` 跳过 Automake 的预生成阶段。若曾遇到
`ocispec/runtime_spec_schema_config_schema.h` 缺失，使用修复后的脚本重新执行即可，
无需删除 `.deps` 或另行安装 OCI 头文件。

仅使用 OCI 时：

```bash
./tools/build-deps.sh --without-krun
cmake --preset debug -DSANDBOX_ENABLE_LIBKRUN=OFF
cmake --build --preset debug
```

可用 `--directory /path/to/cache`、`--prefix /path/to/prefix` 改变缓存和安装位置；
随后配置 `-DSANDBOX_DEPS_PREFIX=/path/to/prefix`。依赖路径暂不支持空白字符，这是上游
Makefile 的限制；SDK 的安装路径和工作区路径仍可包含空格。
`--offline` 要求源码/submodule、内核和 Cargo 包均已缓存，禁止脚本下载；
`--rebuild` 忽略完成记录并重做构建步骤。脚本拒绝覆盖修改过的已缓存源码。

也可以使用外部开发包：

```bash
cmake --preset debug \
  -DLIBCRUN_ROOT=/path/to/crun-development-prefix \
  -DLIBKRUN_ROOT=/path/to/krun-development-prefix
```

外部 crun prefix 必须包含上述静态库、OCI 生成头文件与 config.h，且全部来自同一次
兼容构建。仅安装 crun 可执行程序不满足要求。CMake 不再把某处的库和另一处的源码
头文件自动混合；切换 prefix 会重新查找。libkrun ABI 1、libkrunfw ABI 5 是当前支持组合，
打包时检查 SONAME，不会把其他 ABI 静默改名。

## 安装与移动资源

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release
```

内部程序为 `agentd`、`sandbox-crun`，启用 VM 时另有 `sandbox-krun`；不再构建
`sandbox-git` 或 `sandbox-io`。libkrun 和固件作为私有副本安装到
`PREFIX/libexec/bbm-sandbox/lib`，通过 `$ORIGIN` 的 RPATH 找到彼此，原依赖库不被修改。
libcrun 静态链接到生命周期工作进程。

SDK 为静态库，不能自动推断最终应用所在位置与 SDK 资源的关系。原安装位置使用
CMake 配置的路径；整体移动后，服务端应显式指定：

```bash
export BBM_SANDBOX_RESOURCE_DIR=/new/prefix/libexec/bbm-sandbox
```

这是可信宿主服务的部署配置，不接受 Agent 请求指定，也不搜索调用程序旁边或 PATH。
移动资源还需要满足目标机器的 glibc/系统动态库、体系结构和 KVM 条件；它不是跨发行版
通用静态发行包。SDK 使用者的 CMake 还需要能找到 libgit2、nlohmann_json 与 Threads，
但公共 SDK 头文件不暴露 libgit2/libcrun/libkrun 类型。

## 交给调用方的验证命令

以下命令会实际启动隔离环境；重构时未代替调用方执行这些测试。

```bash
./tools/build-deps.sh --jobs 4
cmake --preset test
cmake --build --preset test

# 先检查基础组件
ctest --preset test -R '^(process-supervisor|git-workspace|sandbox-lifecycle|runtime-backend|agent-protocol|guest-service)$'

# 普通用户下启动 OCI 容器，包含输出回调、取消和文件边界检查
ctest --preset test -R '^manager-crun'

# /dev/kvm 可访问，且用户 cgroup 已委派后启动真实 VM
ctest --preset test -R '^manager-krun'

# 重新编译/安装 SDK，并验证独立应用和资源移动
ctest --preset test -R '^sdk-install$'
```

也可一次执行 `ctest --preset test`。测试需要用户 namespace、cgroup v2 的 memory/pids
委派、静态 BusyBox，以及相应 HostTools 工具；VM 另需 `/dev/kvm`。CPU 未委派时，集成
测试显式关闭 CPU quota。运行不需要 sudo。旧版本已启动的容器没有新 agentd/listener，
不能接管为新协议服务，应使用旧版本回收后重新创建；已停止的工作区记录仍可读取。
