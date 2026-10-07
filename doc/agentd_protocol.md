共享协议同时用于容器和 Guest，当前架构见 [communication.md](communication.md)。

# agentd 协议 v1

本项目使用自己的 `bbm.sandbox.agentd` 协议，借鉴 microsandbox 的固定帧头、CBOR envelope、
请求关联和流式事件设计，不与其 Agent、SDK 或版本号互通。参考：
[帧编码](https://github.com/superradcompany/microsandbox/blob/main/crates/protocol/lib/codec.rs)、
[版本设计](https://github.com/superradcompany/microsandbox/blob/main/crates/protocol/VERSIONING.md)。

## 模块

| 文件 | 职责 |
|---|---|
| `../include/ipc/protocol.hpp/.cpp` | 帧、CBOR、大小/复杂度检查、基础字段验证 |
| `../include/ipc/transport.hpp/.cpp` | 字节传输接口、socket/字符设备适配、deadline、连接失效 |
| `../include/virtualization/agentd_client.hpp/.cpp` | Host 客户端、握手、执行/取消、文件读写 |
| `../include/agentd/service.hpp`、`../src/agentd/service.cpp` | Guest 请求状态机、执行监督、未完成写入的回收 |
| `../src/agentd/main.cpp` | 内部 `agentd` 可执行程序、vsock 监听或专用串行端口 |
| `../include/workspace/workspace_files.hpp/.cpp` | 容器和 microVM 内 agentd 共用的 runtime 内文件边界与原子写入 |

当前默认 `Sandbox` 使用 libcrun，也支持 libkrun 后端。`AgentdClient` 负责请求与响应，
不负责创建 VM、配置资源、销毁 sandbox 或导入工作区。
旧 Firecracker 声明、专用 `CONNECT` 握手和未使用的整树传输草稿已删除。

## 字节格式

```text
[length:u32 BE][id:u32 BE][flag:u8][CBOR envelope]
envelope = {v:uint, t:text, p:bytes}
p = 单独编码的 CBOR map
```

- `length` 不包含自己的 4 字节，包含 ID、flag 和 envelope；最小 5，最大 65536。
- `id=0` 只用于握手；普通请求 ID 从 1 递增，同连接内不得重复。
- `flag=1` 表示请求，`0` 表示事件，`2` 表示最终响应；其他值拒绝。
- `v=1` 是本项目协议版本；不兼容版本直接拒绝。
- 普通 map 可带未知可选字段；未知操作返回 `core.error/unsupported`。
- `p` 和文件/输入/输出数据使用 CBOR byte string，不是整数数组、Base64 或嵌入文本。
- 解码前拒绝重复 map 键、尾随数据、不定长容器、非文本 map 键、无效 UTF-8 文本、tag、浮点值，
  限制深度 24、节点 8192。大小在分配帧 body 前校验。
- v1 每块数据最多 16384 字节；偏移必须与已接受字节数一致。

## 握手与限制

Host 先发送 ID 0 的 `core.hello`，Guest 用 `core.ready` 最终响应。
双方声明 `protocol`、`version`、`file_bytes`、`stdin_bytes`、`output_bytes`、`timeout_ms`。
Guest 对每项使用 Host 请求与可信 Guest 策略的较小值；所有限制必须非零。
Ready 还列出 `exec`、`exec.cancel`、`fs.read`、`fs.write` 能力，Client 检查这些能力。

默认文件、stdin、总 stdout/stderr 各 8 MiB，命令最大超时 60 秒。可信配置最高可设置
64 MiB 和 1 小时；单次命令可以请求更小的超时和输出上限。argv 最多 1024 项、总计
32 KiB，执行文件和 cwd 使用 Guest 绝对路径。不会继承 Host 环境变量或控制连接 FD。

## 操作状态机

同连接一次处理一个普通操作，Host 客户端用锁串行化。执行中允许另一线程发送取消事件。

| 方向 | 消息 | payload |
|---|---|---|
| Host → Guest | `exec.start` 请求 | argv、cwd、stdin_bytes、timeout_ms、output_bytes |
| Guest → Host | `exec.accepted` 事件 | 空 map，允许上传 stdin |
| Host → Guest | `exec.stdin` 事件 | offset、data |
| Host → Guest | `exec.stdin.end` 事件 | 空 map，收到完整输入才启动命令 |
| Guest → Host | `exec.started` 事件 | 空 map，进入执行阶段 |
| Guest → Host | `exec.stdout` / `exec.stderr` 事件 | 各自的 offset、data |
| Host → Guest | `exec.cancel` 事件 | 空 map，使用被取消 exec 的 ID |
| Guest → Host | `exec.exited` 最终响应 | code、timed_out、output_limited、cancelled |

stdin 先有界收集，再交给子进程；v1 暂不支持交互式 stdin 或 PTY。
Guest 接收控制事件的线程与执行线程分离，持续输出时也能接受取消。
`cancel()` 在 Client 收到 started 后生效，否则返回 false；取消和正常退出竞争时以实际最终
结果为准，没有独立取消确认。过期取消只关联原任务，不会影响下一条命令。

| 方向 | 消息 | payload |
|---|---|---|
| Host → Guest | `fs.read` 请求 | path、limit |
| Guest → Host | `fs.read.data` 事件 | offset、data |
| Guest → Host | `fs.read.done` 最终响应 | size |
| Host → Guest | `fs.write` 请求 | path、size |
| Guest → Host | `fs.write.accepted` 事件 | 空 map |
| Host → Guest | `fs.write.data` 事件 | offset、data |
| Host → Guest | `fs.write.end` 事件 | 空 map |
| Guest → Host | `fs.write.done` 最终响应 | size |

文件 API 只接受 workspace 内绝对路径。openat2 使用 BENEATH、NO_SYMLINKS、NO_XDEV；
拒绝 `..`、NUL、特殊文件和硬链接写入目标。父目录必须存在，缺少 openat2 时失败。
`.git` 在 B 中属于任务文件，可以操作；不应把 Manager 的 Git 元数据共享给 Guest。
写入有界地暂存到同一父目录，只在收到所有声明字节和 end 后 fsync、原子 rename。
中断、偏移错误、大小错误或超时会删除暂存文件。rename 之后若响应丢失，Host 必须把结果
视为未知，不能声称已回滚；该机制也不承诺掉电后的目录项持久性或并发编辑冲突检测。

`core.ping` 请求以 `core.pong` 最终响应完成。操作错误以同 ID 的 `core.error`
最终响应返回，包含 code/message。有效的请求拒绝可保留连接；畸形帧、非法事件顺序和
传输超时使连接失效。命令退出码非零是正常结果。

## 传输与用法

libkrun 后端由可信 launcher 把 Guest vsock 端口映射到每个 sandbox 的私有 Host Unix socket。
Host 直接连接该 Unix socket，不发送 Firecracker 的文本握手。连接路径不能来自 Agent 请求。

```cpp
#include <virtualization/agentd_client.hpp>

auto transport = ipc::connect_unix(
    trusted_session_socket,
    ipc::Clock::now() + std::chrono::seconds(5));
virtualization::AgentdClient client(std::move(transport));
client.handshake();
client.write("/workspace/main.cpp", contents);
auto contents_again = client.read("/workspace/main.cpp", 8 * 1024 * 1024);

RuntimeCommand command;
command.argv = {"/usr/bin/git", "status", "--short"};
command.cwd = "/workspace";
auto result = client.execute(command, [](bool is_stderr, std::string_view chunk) {
    // 增量输出；回调应及时返回，不能在回调中调用串行 read/write/exec。
});
// 执行中可以从另一线程 client.cancel()。
```

Guest 内部组件位于构建/安装目录 `libexec/bbm-sandbox/agentd`：

```text
agentd --serve --workspace /workspace --vsock-port 1024
agentd --serve --workspace /workspace --serial /dev/virtio-ports/sandbox.agentd
```

vsock 监听器只接受 Host CID 2。串行模式要求专用 virtio 字节端口，不能与启动日志或终端混用。
`--fd` 为受信 launcher/端到端测试接收已连接 socket 的入口，不是普通用户的宿主机执行模式。
同一协议通过 `Transport` 适配两类通道，未加入双端口、大块数据专用格式或断线重放。

## 隔离与验收范围

Guest 服务本身不能建立硬件隔离。真实后端仍需实现 VMM 工作进程、Host namespace/cgroup
限制、Guest 工具环境、监督进程和任务身份分离、控制端点保护、网络策略、工作区冻结与同步。
关闭继承 FD 和检查 CID 不能代替这些措施；同 UID 的不可信任务仍可能干扰服务进程。
任务创建新进程组/会话后的后台进程也需要 Guest cgroup 或整个 VM 的回收来覆盖。
Host 必须独立保留截止时间，Guest 失联后停止整个 VM，不能仅相信 Guest 自报的限制状态。

`agentd-protocol` 验证固定样本、畸形 CBOR、分片、超大帧及部分帧超时。
`agentd-service` 通过 socketpair 启动真实 Guest 可执行程序，覆盖二进制传输、流式输出、
退出码/cwd、并发取消、超时/输出上限、文件边界、断线写入回收和断线命令回收。
这些是本机进程与协议测试，尚未实际运行 libkrun/KVM 或 virtio 字符设备驱动。

## VM 工作区冻结扩展

真实 libkrun Guest 的 ready.runtime 包含 isolation=libkrun、task_uid=65534、task_pids 和
network=disabled。Host 在首次启动校验资源策略；基础协议测试 Guest 不声明 VM 隔离。

`workspace.freeze` 请求携带 `{frozen: bool}`，以同 ID 的 `workspace.frozen` 最终响应确认。
true 表示冻结 Guest 任务 cgroup 并对 B 执行 syncfs；false 表示解冻任务。
只能在命令与文件操作均完成时调用。不支持 VM bootstrap 的 Guest 返回操作错误。
Host 在收到冻结确认后才能暂停整个 VMM；恢复必须先解冻 VMM，再发送 false。
B 由独立 virtiofs 共享；不通过 RPC 导出 Manager 元数据或整树文件。
