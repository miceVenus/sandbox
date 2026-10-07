# C++ SDK 使用样例

`sdk_example.cpp` 自动创建一个临时 Git 源仓库，用 SDK 创建独立 B 工作区，演示：

1. `create/status` 创建并查询虚拟化环境。
2. `read/write` 读取和修改沙箱内的 `main.cpp`。
3. `execute` 在沙箱内用 G++ 编译、执行程序，并通过回调显示输出。
4. `CommandRequest` 传入相对 cwd 和标准输入。
5. `get_changes` 查看相对基线的状态和补丁，验证宿主源文件未改变。
6. `stop` 保留 B，随后 `destroy` 丢弃 B 并清理本次临时目录。

样例使用 HostTools 环境，宿主机需要 `/usr/bin/git` 和 `/usr/bin/g++`，以及 SDK 的运行时依赖。
普通用户直接运行；microVM 模式还需要 `/dev/kvm` 访问权限和启用了 libkrun 的 SDK。
当前机器未委派 CPU controller，样例显式设置 `cpu_quota_us = 0`；内存、进程数等限制仍生效。
microVM 的 512 MiB 总预算中，一半配置为 guest RAM，另一半预留给 VMM 和共享缓存。

## 在项目内体验

从项目根目录执行：

```sh
cmake --preset debug
cmake --build build/debug --target sandbox-example -j4

# 默认模式，也可以省略 container 参数。
./build/debug/examples/sandbox-example container

# 用同一段 SDK 调用体验 microVM。
./build/debug/examples/sandbox-example microvm
```

样例目标不参与常规 SDK 构建，也没有增加新的构建预设。
`examples/sdk_example.cpp` 中的 Options、后端选择和 API 调用可直接修改后重新构建。

## 作为独立应用链接安装版 SDK

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release

cmake -S examples -B build/example-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/install;$PWD/build/vcpkg_installed/x64-linux"
cmake --build build/example-consumer -j4
./build/example-consumer/sandbox-example container
```

默认安装位置由 release 预设指定，示例 CMake 使用 `find_package(bbm-sandbox)` 和
`bbm::sandbox_core`；无需自行拼接 SDK、libgit2 和运行时助手的链接参数。

## 预期结果

编译后的程序输出 `Hello from the sandbox!`；diff 中显示 greeting 的修改，宿主的原始文件仍输出
`Hello from the source workspace!`。样例不会将 B 自动合并回 A。
成功时清理本次临时目录；失败时尝试回收已创建的 sandbox，并打印保留的临时目录，方便查看记录。
