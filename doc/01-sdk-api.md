# SDK API

[Client](02-clients.md) · [Session 与 IPC](03-session-ipc.md) · [Workspace](04-workspace.md) · [后端](05-backends.md) · [开发环境](06-development-environment.md) · [TODO](07-todo.md) · [系统编程接口](08-system-programming.md)

## 对象与生命周期

一个 `Sandbox` 绑定一个工作区和一个运行环境。默认后端为 libcrun；通过构造函数注入 `make_krun_backend()` 可选择 microVM。多个沙箱使用多个对象。

`create()` 创建并绑定，`open(id)` 打开已有记录。打开时使用原管理目录和相同后端。释放 C++ 对象会释放通信连接；运行环境和 B 工作区通过 `stop()`、`destroy()` 显式回收。这使 SDK 应用能够退出后重新接管沙箱。

默认管理目录为 `$XDG_STATE_HOME/bbm-sandbox`，未设置时为 `~/.local/state/bbm-sandbox`；root 用户为 `/var/lib/bbm-sandbox`。构造函数可指定其他目录。

## 接口行为

| API | 行为 |
|---|---|
| `create(options)` | 校验配置，从源 Git 提交创建 B，准备环境、启动后端并核对资源限制，返回 `SandboxInfo` |
| `open(id)` | 读取持久记录并绑定；后续运行时查询和操作按需建立连接 |
| `id()` / `info()` | 返回绑定的 ID / 当前记录 |
| `execute(request, callback)` | 执行命令，接收流式输出并返回 `Result` |
| `execute(argv, callback)` | 使用默认 cwd 和空 stdin 执行命令 |
| `cancel()` | 向当前执行发送取消事件；返回是否发送，最终结果看 `Result.cancelled` |
| `read(path)` | 读取工作区内文件，返回含二进制内容的 `std::string` |
| `write(path, content)` | 在工作区内创建或替换文件；父目录须已存在 |
| `status()` | 返回业务记录、后端状态和运行时错误；发现 Active 环境失效时更新为 Failed |
| `get_status()` | 返回 `status().info.state` |
| `get_changes()` | 暂停、同步、读取状态和补丁，再恢复运行；停止后的 B 也可预览 |
| `stop()` | 停止运行环境，保留 B、运行数据和记录，可重复调用 |
| `destroy()` | 回收并确认后端停止，删除沙箱目录，标记 Discarded，可重复调用 |

`destroy()` 后句柄保留 ID 和最终信息，不能用于创建另一个沙箱。

## Options

| 字段 | 默认值 | 用途 |
|---|---|---|
| `src_repo` | 必填 | 源 Git 工作树路径；可传仓库内子目录，最终解析到仓库根 |
| `revision` | `HEAD` | B 的起始提交 |
| `ctr_repo` | `/workspace` | 隔离环境内工作区挂载位置 |
| `cwd_rlt` | `.` | 相对工作区的初始工作目录 |
| `environment` | `HostTools` | 借用宿主工具目录或使用 Minimal 工具集 |
| `memory_bytes` | 256 MiB | 内存预算；microVM 包含 VMM 开销 |
| `cpu_period_us` / `cpu_quota_us` | 100000 / 100000 | 一核 CPU 配额；quota 为 0 表示关闭配额 |
| `max_tasks` | 64 | 进程数上限 |
| `cmd_timeout` | 2000 ms | 单条命令期限 |
| `max_output_bytes` | 8 MiB | stdout 与 stderr 总上限 |
| `max_file_bytes` | 8 MiB | 文件及 stdin 大小上限 |
| `policy` | `ReviewOnly` | 当前仅支持预览；`AutoFastForward` 创建时被拒绝 |

源工作树须已提交且干净，当前不支持 submodule。`SandboxInfo` 记录 ID、后端标识、目录、配置、基线、源 HEAD、源分支和业务状态。

## 命令、文件和结果

`CommandRequest` 包含 `argv`、可选 `cwd_relative`、二进制 `stdin_data`。执行文件须使用隔离环境内的绝对路径；cwd 相对 B，默认取 `cwd_rlt`。argv 直接传给程序。需要 shell 语法时，显式执行 `/bin/sh -c`。

`Result` 包含 `runtime_status`、`out`、`err`、`timed_out`、`output_limited`、`cancelled`。程序非零退出码作为结果返回；参数、文件和传输错误通过异常或文件 API 的失败检查报告。

回调在 `execute()` 调用线程中同步执行，输出也累计到 Result。分块可能跨行或跨 UTF-8 字符；stdout、stderr 各自有序。回调内可调用 `cancel()`，其余阻塞 API 使用同一沙箱锁，不能重入。普通操作串行，忙时报告 `sandbox busy`；取消可由另一线程发起。

read/write 接受 B 内的绝对路径，包括 B 自己的 `.git`。路径解析使用 `openat2`，拒绝 `..`、符号链接、跨挂载访问和特殊文件；write 拒绝硬链接目标。写入通过同目录暂存文件、fsync 和 rename 完成。读取上限取文件与输出限制的较小值。

## 状态

主要流程为 `Preparing → Active → Stopped → Discarded`。创建失败、执行通信异常、命令超时或输出超限进入 `Failed`；超时和输出超限会回收运行环境并保留 B。取消本身不把沙箱标记为 Failed。

枚举中的 `Frozen`、`Committed`、`Applied` 为结果交付阶段预留。当前 API 提供改动预览，尚未提供 merge 或最终提交固定。

实现入口：[sandbox.hpp](../include/sandbox.hpp)、[sandbox_types.hpp](../include/sandbox_types.hpp)、[sandbox.cpp](../src/sandbox.cpp)。完整调用见 [样例](../examples/sdk_example.cpp)。
