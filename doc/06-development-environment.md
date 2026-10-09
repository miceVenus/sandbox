# 虚拟运行环境的开发工具

容器与 microVM 共用 rootfs 准备逻辑，提供 HostTools 和 Minimal 两种环境。默认 HostTools，借用本机工具链，并为每个沙箱分配独立的可写数据目录。

环境准备代码集中在 `virtualization/environment/`，通过 `environment.hpp` 声明普通函数。`make_config()` 一次生成工具挂载列表和环境变量；`prepare_rootfs()` 创建目录、安装程序及其依赖；`make_oci_config()` 在内存中构造 OCI JSON，由后端设置启动程序后写入最终文件。MicroVM 直接准备 Guest rootfs 和配置，外层容器单独构造 OCI JSON。cgroup 委派检查和资源验证位于 `virtualization/container/resources.cpp`。

## HostTools 的准备流程

1. 创建轻量 rootfs 和必要挂载点，安装 SDK 的 agentd。
2. 枚举本机工具目录，生成对应只读挂载。
3. 重建 merged-/usr 链接和指向工具目录的 alternatives 链接，生成最小 passwd、group、nsswitch 和 hosts。
4. 创建独立 runtime-data，并映射为 `/build`、`/env`、`/cache`。
5. 挂载 B，启动 agentd。

工具目录包括本机存在的 `/usr/bin`、`/usr/lib`、`/usr/lib64`、`/usr/libexec`、`/usr/include`、`/usr/share`、`/bin`、`/lib`、`/lib64`。普通目录使用非递归 bind，只读挂载；目录别名在 rootfs 中重建。宿主 home、完整 `/etc`、`/run`、`/usr/local` 不导入。

容器直接使用这些挂载。microVM 先把它们只读挂入 VMM 的 `/guest-root`，再由 libkrun 通过只读 virtiofs root 提供给 Guest；B 和 runtime-data 使用另外两份可写共享。Guest `/tmp` 为独立 tmpfs。

## 可写目录与依赖安装

| 路径 | 内容 | 生命周期 |
|---|---|---|
| 工作区，默认 `/workspace` | 源码和任务 Git 仓库 | Git 预览范围 |
| `/build` | 编译产物与 tmp | stop 后保留，destroy 后删除 |
| `/env` | HOME、Python venv 等依赖环境 | stop 后保留，destroy 后删除 |
| `/cache` | pip 等工具缓存 | stop 后保留，destroy 后删除 |

HostTools 的任务环境设置 `PATH=/env/python/bin:/usr/bin:/bin`、`HOME=/env/home`、`TMPDIR=/build/tmp`、`XDG_CACHE_HOME=/cache`、`PIP_CACHE_DIR=/cache/pip`，并要求 pip 使用 venv。

例如在沙箱内执行：

```sh
/usr/bin/python3 -m venv /env/python
/env/python/bin/pip install --no-index --find-links /workspace/wheels package_name
/usr/bin/g++ /workspace/main.cpp -o /build/main
```

Python 包安装到 `/env/python`，缓存进入 `/cache/pip`，不会进入源码 diff。当前没有外网，依赖需通过 B 中的离线包或已导入系统工具提供。

## 为什么选目录导入

把宿主 `/` 整体只读挂入也能运行编译器，但会让任务读取 home、服务配置和其他项目。只读限制写入，不限制读取。当前选择指定工具目录，让工具和源码来源分开。

预装工具的 rootfs 镜像可以固定版本、独立于宿主，并适合分发。它同时要求维护工具版本、镜像构建和更新流程。当前优先本机 SDK 开发体验，目录导入无需维护完整系统模板；代价是工具版本随宿主变化，导入目录的内容对任务可见。

单独复制编译器可缩小导入范围，但还需维护动态库、头文件、编译器内部程序和资源目录。HostTools 保留系统工具布局，减少这类适配。自定义工具目录与版本锁定仍在 TODO 中。

## Minimal

Minimal 复制静态 BusyBox 并创建 applet 链接，安装 Git、agentd 及所需动态库。程序依赖由 environment 模块的 `install_program()` 解析并复制。它适合 shell、Git 和文件操作，不提供完整 C++/Python 开发工具链。

Minimal 不需要一份完整发行版镜像，但需本机静态 BusyBox、Git 和依赖解析工具。HostTools 用于编译体验，Minimal 用于小工具集任务。

内核与工具环境分开准备：libkrunfw 提供 Guest 内核，rootfs 提供用户态程序。运行时依赖由 `deps.lock.json` 固定源码和固件版本，`tools/build-deps.sh` 构建到 `.deps/prefix`；这不会固定 HostTools 的系统包版本。

实现：[host_tools.cpp](../src/virtualization/environment/host_tools.cpp)、[rootfs.cpp](../src/virtualization/environment/rootfs.cpp)、[microvm_bootstrap.cpp](../src/agentd/microvm_bootstrap.cpp)。
