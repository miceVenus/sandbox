# 两个 Client

`AgentdClient` 负责隔离环境内部的任务请求，`ContainerClient` 负责宿主侧的容器生命周期。两者由后端组合。

```text
Sandbox → RuntimeBackend
             ├─ AgentdClient → Session → agentd → 命令 / 文件操作
             └─ ContainerClient → libcrun 工作进程 → 容器生命周期
```

## AgentdClient

agentd 常驻在 sandbox 内。容器中它是 PID 1；microVM 中它在 Guest 内启动，并配置任务身份和 cgroup。文件操作由服务内函数处理，命令通过子进程执行。

AgentdClient 持有 Session，负责握手、隔离类型校验、启动建连重试、请求 ID、exec、输出事件、取消、分块文件读写、ping、工作区冻结和部分错误到 Result 的转换。`ipc::Session` 接收已经建立的连接，负责消息分帧收发、发送互斥、断开与失效处理；字节 I/O 由 `ipc::SocketStream` 提供。

选择常驻服务是因为命令与文件操作频繁，且都需要相同的路径、身份和资源策略。每次调用 libcrun exec 会重复创建运行时工作进程，也不能直接复用到 VM。单独启动文件助手则会重复实现启动、监督和数据传输。常驻 agentd 把这些操作放在一套服务逻辑中。

## ContainerClient

提供 `start/state/pause/resume/destroy`。OCI 后端用它管理任务容器；microVM 后端用它管理承载 VMM 的外层容器。

每次调用执行 SDK 内部的 `sandbox-crun` 工作进程。该程序链接 libcrun，直接调用其 API；请求通过封印 memfd 交付，stdout/stderr 管道返回状态和诊断，进程退出码表示执行结果。

启动容器时，还传入已打开的 Unix listener FD。工作进程读取并关闭请求 FD，再把 listener 交给容器 agentd。回收依次请求 kill 和 delete，由后端核对最终状态。

## 为什么保留独立 libcrun 进程

容器启动涉及进程创建、namespace、文件描述符、信号和运行时环境；宿主 SDK 则可能嵌入多线程服务。独立工作进程把这些操作限制在专门的进程边界中，进程退出后释放其局部状态，宿主还可独立施加期限。

SDK 直接嵌入 libcrun 可以省去一次进程启动，但需要把库的进程环境要求纳入整个宿主程序的约束。当前选择 SDK 私有工作进程，调用方只依赖 Sandbox API。

常驻生命周期 daemon 也能减少启动次数，但需要管理 daemon 自身的启动、状态、重连和回收。生命周期操作频率较低，当前一次请求一个进程更直接。任务高频通信已经由 agentd 承担。

生命周期不能依赖 sandbox 内的 agentd：服务失联、Guest 卡住或通信损坏时，宿主仍须暂停和销毁整个环境。因此两类 Client 保留不同的控制路径。

实现：[agentd_client.cpp](../src/virtualization/agentd_client.cpp)、[container_client.cpp](../src/virtualization/container_client.cpp)、[crun_worker.cpp](../src/virtualization/container/crun_worker.cpp)、[crun_ops.c](../src/virtualization/container/crun_ops.c)。
