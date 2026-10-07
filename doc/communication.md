# 共享通信与常驻任务服务

容器和 microVM 共用 `ipc/protocol` 的 CBOR 消息、分帧、请求 ID 和事件，
共用 `ipc/transport` 的字节传输、截止时间和失效处理，
共用 `agentd/service` 的请求处理、执行监督和基于 openat2 的文件访问。

同一个运行环境一次由一个宿主连接持有，容器和 VM 服务都串行接待连接。重新打开仍在
运行的沙箱前，先析构原 `Sandbox` 对象以释放其连接；这不会停止运行环境或删除 B。
低层协议测试也需要先释放 SDK 连接，再接入测试连接，断开后才能由 SDK 重新接管。
同一进程中的多个线程应使用同一个 `Sandbox`；当前不支持多个句柄同时持有同一个环境。

`virtualization/Session` 持有一个长期连接。只有建立连接和 ready 握手可以在
启动阶段重试，`exec`、`write` 等操作不会在断线后重放。连接没有默认空闲断开期限。
服务的输出发送直接使用有限大小帧和有期限的写操作，不维护无界发送队列。

## 容器

宿主先创建位于私有 `control` 目录的 Unix listener，把开放 FD 经 SDK 内部
libcrun 工作进程传给容器 PID 1。socket 的文件路径不挂载到容器中。PID 1 运行 agentd，
接受宿主连接、执行 builtin 文件操作，并通过 posix_spawn 创建命令子进程。

agentd 设置不可 dumpable，任务不能继承监听或连接 FD；同 PID namespace 内的普通任务
不能给未注册处理信号的 PID 1 强制终止。每次任务结束或连接异常，都回收任务进程组，
再由 PID 1 清理 namespace 内残留进程（包括 setsid 后的孤儿）。进程清理需要确认完成。
容器 user namespace、空 capabilities、noNewPrivileges、只读 rootfs 和宿主 cgroup 限制
继续由 OCI 配置承担。这里没有将 agentd 提升为宿主 root。

exec/read/write/ping 使用长期连接。libcrun 工作进程只处理启动、状态恢复、暂停、恢复和
销毁，接收独立 control FD 中的结构化请求，不提供 crun 风格命令行，也不调用外部 crun。
普通活动状态查询优先通过 agentd ping 验证，避免每个命令再启动一次状态工作进程。
最终回收独立于 agentd，连接故障也能通过 libcrun 销毁整个环境。

`ContainerClient` 是后端向容器发出生命周期请求的内部组件，提供 start/state/pause/resume/
destroy。OCI 后端通过它管理任务容器；microVM 后端通过它管理承载 VMM 的外层保护容器。
它目前使用 libcrun 工作进程与私有协议，执行命令和文件读写由独立的 agentd 连接处理。

## microVM

任务服务使用相同程序和协议；VM bootstrap 单独负责 virtiofs、UID 映射和 Guest cgroup。
Guest task 子模式仍由 agentd 的自身可执行程序承载，在独立进程中进入任务 cgroup、降权
并 exec；它不是一个对外 CLI。Host 通过 libkrun 的 Unix/vsock 代理连接 Guest 服务。
Guest 任务和 Host VMM 的资源回收由各自后端负责，不属于通用 transport 的职责。

## SDK 输出与取消

```cpp
std::string log;
auto result = sandbox.execute(
    std::vector<std::string>{"/bin/sh", "-c", "echo running; sleep 1; echo done"},
    [&](OutputStream stream, std::string_view bytes) {
        // bytes 是二进制分块，可能不是完整行或 UTF-8 字符；只在回调期间有效。
        if (stream == OutputStream::Stdout) log.append(bytes);
    });
```

回调同步运行在 execute 的调用线程；输出同时保留在受总量限制的 Result 中。
stdout/stderr 分别保持各自顺序，不保证两个独立管道之间的精确时序。
回调应及时返回；消费过慢导致发送超时会关闭连接、取消任务，并使 SDK 回收失败环境。
回调异常也会终止执行并记录 Failed，不能重放任务。

`sandbox.cancel()` 可在输出回调或另一线程调用，只表示取消事件是否已发送；以最终
`Result.cancelled` 确认结果。其余阻塞 API 不支持在 execute 回调中重入，单个 Sandbox
仍串行执行任务。没有被任务写入管道的缓冲输出无法由 SDK 提前传递。

## Git

`Sandbox` 直接使用 `Workspace`，其 Git 操作调用 libgit2；已删除工作区后端接口、
适配器与工厂。创建时读取仓库路径和基线，预览时在运行时冻结、同步后生成状态和补丁。
不再存在 Git 工作进程、命令行解析或 JSON 结果桥接。
仓库配置独立、资源用 RAII 回收，仍以私有基线比较 B，并拒绝不支持的仓库形式。
对象导入检查期限；不再通过杀死进程提供所有 Git 操作的硬超时。
