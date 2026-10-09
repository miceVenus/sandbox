# Session 与 IPC

Runtime 持有 AgentdClient，AgentdClient 和 agentd Service 共用 ipc::Session；Session 直接持有 ipc::SocketStream，由 UniqueFd 管理 FD 生命周期。工作区与运行环境的生命周期由 Sandbox 和后端管理。

Session 接收已建立的连接，负责持有、分帧发送、接收、发送互斥、断开与失效处理，不依赖 AgentdClient，也不提供执行或文件操作接口。AgentdClient 负责握手、能力协商、运行时身份检查、请求 ID 和具体消息流程；启动重试仅发生在 Client 建连阶段。ipc::connect_unix() 和 accept_socket() 负责建立连接，Session 通过构造函数或 attach() 接收 UniqueFd。

## 通道建立

容器：宿主在沙箱私有 `control/` 中创建 Unix domain socket listener，通过 libcrun 工作进程把 FD 传给 PID 1 agentd。socket 路径留在宿主，任务子进程关闭控制 FD。

microVM：libkrun 把 Guest vsock 端口 10789 映射到宿主 `control/agentd.sock`。SDK 连接 Unix socket，Guest agentd 接受 Host CID 2 的 vsock 连接。启动日志走独立 console 文件。

两种后端在 SDK 侧都调用 `AgentdClient::connect()`，完成 `core.hello → core.ready` 握手并核对 isolation。启动阶段允许重试建连；已发送的操作不重放。当前一个运行环境由一个连接持有，重新接管前先释放旧句柄。

## 为什么使用 socket

| 方案 | 特点 | 当前选择 |
|---|---|---|
| Unix domain socket | 全双工、可命名连接、支持连接与进程生命周期分离 | SDK 到 agentd 的常驻通道 |
| 匿名管道 | 单向；双向需两条，FD 通常由父子进程继承 | 短生命周期工作进程的输出和诊断 |
| TCP | 需要端口和网络配置 | 本机控制和 Guest 通信已有专用通道 |
| 共享内存 | 需额外实现通知、同步、队列和连接管理 | 当前有界分块传输无需这些机制 |

管道也能承载双向协议，但 agentd 会跨 SDK 句柄存活，`open()` 需要重新连接。匿名管道不能仅凭路径重新连接现有服务；Unix socket 的 listener 正好支持这一需求。

ContainerClient 的请求实际上走 memfd，返回值走管道。请求只有一次，工作进程结束即完成交互，退出码与 EOF 就能表达结束。把 agentd 套进这个模型，会失去常驻连接和重接能力；把生命周期工作进程改为常驻 socket 服务，则增加服务管理成本。

Guest 选择 vsock 是因为控制通信不需要 IP 网络。libkrun 提供 Unix/vsock 映射，可直接接入宿主 Session。TCP 会额外引入 Guest 网卡、地址和路由配置。

## 帧与消息

```text
[length:u32 BE][id:u32 BE][flag:u8][CBOR envelope]
envelope = { v: 1, t: 消息类型, p: CBOR 编码的 payload 字节 }
```

length 包含 ID、flag 和 envelope，上限 64 KiB；数据块上限 16 KiB。ID 0 用于握手，普通请求递增。flag 区分请求、事件和最终响应。

CBOR 直接表达二进制 stdin、文件和输出，省去 JSON 的 Base64 编码。JSON 更便于人工查看，但这里是内部二进制传输。Protobuf 可提供生成类型，当前消息量较小，CBOR 与现有 nlohmann_json 共用依赖。帧头负责边界，类型与 ID 负责路由，两者与底层传输分开。

| 操作 | 消息顺序 |
|---|---|
| 握手 | `core.hello → core.ready`，协商能力和限制 |
| 执行 | `exec.start → accepted → stdin/end → started → stdout/stderr → exited` |
| 取消 | 执行中发送同 ID 的 `exec.cancel`，由 exited 返回最终标志 |
| 读取 | `fs.read → data → done` |
| 写入 | `fs.write → accepted → data/end → done` |
| 探活 | `core.ping → core.pong` |
| VM 冻结 | `workspace.freeze → workspace.frozen` |

同连接普通操作串行；执行中可发送取消。stdin 先收集再启动任务，当前没有交互式 stdin 或 PTY。输出按事件发送，发送期限和总量限制提供背压。

agentd 的 `ServiceConfig` 由 bootstrap 填充，`agentd::serve()` 接管连接。Service 分别持有 Task 和可选 Upload：Task 保存命令、stdin、进程配置与执行线程；Upload 只记录上传种类、大小、偏移和期限，文件上传另持有 Write 事务。执行成功、启动失败或输出发送失败都进入一次任务清理，再发送最终响应；最终响应发送失败不会再次清理。WorkspaceFiles 自己管理文件访问身份和写入事务的 FD，未提交事务析构时移除临时文件。

## 失效处理

有效请求的操作错误用 `core.error` 返回。畸形帧、部分帧超时或断线使通道失效。写入完成但响应丢失时结果可能已生效，自动重放会改变操作语义，因此只在建连阶段重试。

封印 memfd 用于一次性请求，使内容在交付后保持固定；数据不会与任务 stdin 或诊断混合。常驻协议使用帧大小、解码复杂度、偏移和期限检查来约束持续交互。

实现：[session.cpp](../src/ipc/session.cpp)、[protocol.hpp](../include/ipc/protocol.hpp)、[protocol.cpp](../src/ipc/protocol.cpp)、[io.cpp](../src/ipc/io.cpp)。
