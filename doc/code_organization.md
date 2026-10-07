# 代码组织与命名约定

Sandbox 表示一个虚拟化环境，其实现可以是 container 或 microVM。一个 SDK Sandbox 对象绑定一个工作区和一个运行时。
Session 只表示 SDK 与 sandbox 内 agentd 的通信通道，不表示工作区、任务或虚拟化环境。

agentd 是 sandbox 内常驻的服务进程。AgentdClient 是 SDK 侧向 agentd 发出请求、处理响应的客户端。
AI Agent 表示使用 SDK 的外部调用方；项目内部不使用模糊的 agent 类、变量或命名空间。

## 职责与目录

| 目录或文件 | 职责 |
|---|---|
| `include/` / `src/` | 声明与实现使用对应的目录和文件名 |
| `sandbox.hpp` / `sandbox.cpp` | 对外 SDK、生命周期编排、状态与持久记录 |
| `workspace/` | B 工作区、Git 基线和变更预览，以及工作区文件边界 |
| `virtualization/runtime.hpp` | 可替换的虚拟化后端契约 |
| `virtualization/container/` | libcrun 容器实现及独立宿主 worker |
| `virtualization/microvm/` | libkrun VM 实现及独立宿主 launcher |
| `virtualization/container_client.*` | 向宿主 worker 发出容器生命周期请求，供两个后端使用 |
| `virtualization/session.*` | 持有与 agentd 的长期连接，统一执行、读写与响应错误处理 |
| `virtualization/agentd_client.*` | agentd 请求语义、握手、请求 ID、输出事件和取消 |
| `agentd/` | 常驻服务、请求分发、任务子进程及容器/VM 内初始化 |
| `ipc/` | 消息编码、分帧、传输适配和通道失效 |
| `lib/` | 不含具体业务语义的通用操作 |

## 两条独立调用路径

```text
Sandbox → RuntimeBackend → ContainerClient → crun_worker → libcrun
                         → krun_runner → libkrun（microVM）

Sandbox → RuntimeBackend → Session → AgentdClient → IPC → agentd
```

Session 承载 execute/read/write/ping/cancel 等请求。普通请求串行，取消可并发发送。
连接只在建立时握手；重复连接同一 endpoint 返回已有客户端。请求断线后不自动重放。
冻结 guest 内任务属于 agentd 服务策略，暂停/恢复/销毁虚拟化环境属于宿主生命周期管理。
即使 agentd 失去响应，后端仍必须能回收 container 或 VM；Session 不提供 destroy 接口。

## lib 的归属

通用错误检查放在 `lib/error`；字符串与随机 ID 放在 `lib/string`；目录校验与受限路径打开放在 `lib/filesystem`；
FD 所有权放在 `lib/descriptor`；deadline 字节读写放在 `lib/io`；Unix/vsock socket 放在 `lib/socket`；
子进程监督放在 `lib/process`；不可变 memfd 放在 `lib/memory_file`；JSON 写入放在 `lib/json`；动态依赖解析放在 `lib/elf`。

sandbox ID 校验、Git commit 格式要求、工作区文件大小策略、协议帧限制、身份校验和 rootfs 安装策略保留在所属业务模块。
通用函数使用 `lib::`，传输与消息使用 `ipc::`，服务使用 `agentd::`，会话与请求客户端使用 `virtualization::`。

## 命名与实现风格

- 文件、变量和方法使用 snake_case，类型使用 PascalCase；名称表达实际职责。
- 目录表达的上下文无需反复叠加到文件名里，避免无实际用途的中间目录。
- C++ 非 void 函数采用 `auto name(...) -> Type`；C API 保留 C 声明。
- 采用四空格缩进，展开函数和控制流。统一排版规则在根目录 `.clang-format`。
- include 从项目 include 根目录开始，不写 `../../include/`。
- 稳定状态由绑定环境的对象持有；execute/read/write 不重复接收 SandboxInfo。
- 移动或改名同步更新声明、调用方、构建、安装、测试与文档，删除旧实现。

新工作区记录写入 `workspace.txt`；旧 `session.txt` 只在兼容读取处出现。
新沙箱记录写入 `sandbox.json`；旧 `session.json` 只在兼容迁移处出现。
Git 任务分支命名为 `sandbox`，与 agentd 服务进程名称无关。

## 构建边界

`sandbox_core` 包含宿主 SDK 与共享实现；agentd 包含服务实现与共享实现，不链接整个宿主 SDK。
`sandbox_shared` 是内部 object target，用于复用 lib、IPC 和文件边界实现，不新增对外链接要求。
libcrun/libkrun 仍通过独立宿主进程调用。公开构建入口保留 release/debug/test 三个预设。

```sh
cmake --preset test
cmake --build --preset test
ctest --preset test
```

本轮头文件与工厂名有调整：`virtualization/agentd_client.hpp`、`virtualization/microvm/krun_runtime.hpp`、`make_crun_backend()`、`make_krun_backend()`。
运行时状态目录统一使用 `control/agentd.sock`，旧版活动环境应先回收，再用新 SDK 创建。
