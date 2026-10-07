# crun 与 krun 后端

Sandbox 通过 RuntimeBackend 调用环境准备、启动、状态、执行、文件读写、暂停、同步、恢复和停止。工作区由 Workspace 管理，任务协议由 Session 复用。

## 隔离方式

| 项目 | crun：`oci-crun` | krun：`vm-libkrun` |
|---|---|---|
| 隔离边界 | Linux namespace，共享宿主内核 | KVM microVM，独立 Guest 内核 |
| 运行组件 | libcrun 工作进程 + 容器 agentd | 外层 libcrun 容器 + libkrun VMM + Guest agentd |
| 工作区 | bind mount | virtiofs |
| 任务身份 | rootless 下 UID 0 映射宿主当前用户 | Guest UID/GID 65534 |
| 通信 | Unix socket listener FD | Host Unix socket → Guest vsock |
| 资源控制 | 宿主 cgroup | 宿主 VMM cgroup + Guest 任务 cgroup |

容器复用宿主内核，环境准备和启动路径较短；microVM 通过独立内核增加隔离边界，同时需要 Guest 启动、虚拟设备和 VMM 资源。SDK 调用方式相同，调用方按隔离需求选择后端。

## crun

后端生成 OCI bundle，把 B 挂到指定工作目录。rootfs 只读，任务使用空 capabilities 和 noNewPrivileges，隔离 pid、network、ipc、uts、mount、cgroup；rootless 另加 user namespace。

rootless 将容器 UID/GID 0 单映射到调用用户，B 在宿主保持原所有权。systemd 用户 cgroup 施加内存、禁止 swap、进程数和可选 CPU 配额，启动后读取实际限制核对。rootful 路径使用任务 UID/GID 65534。

agentd 作为 PID 1 接收请求，创建命令子进程并回收残留任务。活动状态优先通过 ping 查询；暂停、恢复、回收通过 ContainerClient 调用 libcrun。网络命名空间默认无外网。

选择 libcrun 是为了直接调用 OCI 生命周期 API，保留进程隔离而无需解析外部 crun CLI。runc 同样支持 OCI，但当前 SDK 已围绕 libcrun C API 和依赖构建接入；切换运行时需重新实现生命周期适配。

## krun

宿主先创建 rootless OCI 保护容器，再在其中运行独立 `sandbox-krun` 进程。VMM 只能看到 Guest root、B、runtime-data、control 和 `/dev/kvm` 等配置资源。外层 namespace/cgroup 负责约束 VMM 自身。

libkrun 配置 vCPU、RAM、virtiofs、vsock 和 Guest 启动程序；内核由匹配的 libkrunfw 提供。Guest agentd 挂载共享目录、建立任务 cgroup，再以 UID/GID 65534、空有效 capabilities 和 noNewPrivileges 启动命令。idmapped mount 将共享文件所有权映射给任务，宿主 B 仍归调用用户。

内存总预算至少 256 MiB，一半用于 Guest RAM，另一半预留 VMM 和共享缓存。Host pids 限额为 `max(128, max_tasks)`，Guest pids 限额为 max_tasks。Guest 每条命令完成后使用 cgroup.kill 回收任务及脱离进程组的子进程。

预览顺序为 `Guest 冻结任务 → syncfs → 暂停 VMM → 读取 B → 恢复 VMM → 解冻任务`。B 是独立 virtiofs 共享目录，无需 RPC 导出整棵文件树。停止时回收外层容器，保留 B 和 VM console 日志。

默认关闭网络并关闭 libkrun 隐式 vsock/TSI 代理，只配置控制通道。当前后端要求普通用户、可访问 `/dev/kvm` 和 cgroup 委派。

## 为什么选择 libkrun

libkrun 用库 API 配置 microVM，并通过 libkrunfw 提供内核，契合当前“自动准备环境 + 启动 agentd”的流程。手写 KVM 需要自行实现虚拟设备、内存布局和启动流程；本项目无需承担这一层。

Firecracker 提供独立 VMM 与控制 API，适合后续引入镜像、设备和独立 VMM 生命周期管理，但需另行准备 Guest 内核、rootfs 和控制适配。QEMU 提供更广的设备和启动选项，当前单一任务环境不需要这些配置范围。项目先使用 libkrun，RuntimeBackend 保留后续后端的接入位置。

实现：[runtime.hpp](../include/virtualization/runtime.hpp)、[crun_runtime.cpp](../src/virtualization/container/crun_runtime.cpp)、[krun_runtime.cpp](../src/virtualization/microvm/krun_runtime.cpp)、[krun_runner.cpp](../src/virtualization/microvm/krun_runner.cpp)。
