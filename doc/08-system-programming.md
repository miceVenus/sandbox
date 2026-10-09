# 系统编程接口与系统调用

本文梳理 `src/` 中直接使用的 Linux/POSIX 接口，并补充测试、样例及基准中的用法。每节列出作用、调用方式和实现位置。

接口分三层：系统调用进入内核；libc 接口提供进程、账户和错误处理；libcrun/libkrun 将隔离与虚拟机操作封装为库 API。cgroup、procfs 则通过文件读写配置或查询内核状态。

## 1. 文件描述符与 I/O

FD 是进程中的整数句柄，可引用文件、目录、管道或 socket。项目用 `lib::UniqueFd` 表达独占所有权，析构时关闭，移动时转移所有权。

| 接口 | 调用方式 | 项目作用 |
|---|---|---|
| `open` | `open(path, flags[, mode])` | 打开工作区目录、cgroup 文件、namespace 和 `/dev/kvm` |
| `openat` | `openat(dirfd, name, flags[, mode])` | 相对已打开父目录检查写入目标、创建暂存文件 |
| `close` | `close(fd)` | 释放描述符；关闭管道端还影响 EOF 的出现 |
| `read` / `write` | `read(fd, buf, size)`、`write(fd, buf, size)` | 文件、管道及控制文件的字节传输 |
| `pread` | `pread(fd, buf, size, offset)` | 工作进程按偏移读取 memfd 请求，不改变共享文件偏移 |
| `lseek` | `lseek(fd, 0, SEEK_SET)` | memfd 写完后把文件偏移重置到开头 |
| `dup2` | `dup2(source, target)` | 将容器 listener 从 FD 4 移到约定的 FD 3 |
| `fcntl` | `fcntl(fd, command, argument)` | 设置非阻塞、exec 关闭、复制 FD 和封印 memfd |

`read/write` 返回实际字节数。正数可能小于请求大小，调用者推进偏移；read 返回 0 表示 EOF。`EINTR` 表示被信号中断，项目循环重试；非阻塞 I/O 的 `EAGAIN/EWOULDBLOCK` 转入等待。

### 标志与 FD 继承

| 标志 / 命令 | 含义与用法 |
|---|---|
| `O_CLOEXEC` / `FD_CLOEXEC` | exec 时关闭 FD；前者在创建时设置，后者通过 `F_SETFD` 设置 |
| `O_NONBLOCK` | I/O 暂不可完成时返回；`F_GETFL` 后用 `F_SETFL` 加入该标志 |
| `O_DIRECTORY` | 要求打开目录 |
| `O_NOFOLLOW` | 不跟随路径最后一段的符号链接 |
| `O_PATH` | 获取路径对象句柄，用于 fstat，不读取文件数据 |
| `O_CREAT | O_EXCL` | 仅创建新文件，用于独占生成暂存文件 |
| `F_DUPFD_CLOEXEC` | 在指定最小 FD 以上复制句柄，同时设置 CLOEXEC |

FD 复制后引用相同打开对象。进程监督器先把待继承 FD 复制到目标范围之外，再依次映射到子进程 3、4、…，避免编号重叠时覆盖尚未映射的源 FD。

CLOEXEC 管理“哪些 FD 穿过 exec”，O_NONBLOCK 管理“如何等待 I/O”，二者职责不同。

实现：[descriptor.cpp](../src/lib/descriptor.cpp)、[io.cpp](../src/ipc/io.cpp)、[process.cpp](../src/lib/process.cpp)。

## 2. 进程创建、执行与回收

### posix_spawn：一般命令与内部工作进程

`posix_spawn` 是 libc 接口。项目预先描述子进程的 FD、cwd、进程组和环境，再启动可执行程序。

| 接口 | 用法与作用 |
|---|---|
| `posix_spawn_file_actions_init/destroy` | 创建、释放文件操作列表 |
| `posix_spawn_file_actions_adddup2` | 将管道写端映射为 stdout/stderr，输入 socket 映射为 stdin |
| `posix_spawn_file_actions_addopen` | 没有输入时将 `/dev/null` 打开为 stdin |
| `posix_spawn_file_actions_addchdir_np` | 仅改变子进程 cwd，不改变 SDK 当前目录 |
| `posix_spawn_file_actions_addclosefrom_np` | 关闭指定编号及之后的 FD，保留显式传递的句柄 |
| `posix_spawnattr_init/destroy` | 创建、释放进程属性 |
| `posix_spawnattr_setflags` | 启用 `POSIX_SPAWN_SETPGROUP` |
| `posix_spawnattr_setpgroup` | 设置为 0，让子进程建立以自身 PID 为 ID 的进程组 |
| `posix_spawn` | `posix_spawn(&pid, executable, &actions, &attrs, argv, envp)` |

argv、envp 是以空指针结尾的数组。项目使用绝对执行路径和显式环境，不进行 shell 拼接。`posix_spawn*` 返回错误码本身，错误文本用 `strerror(rc)`，与多数系统调用的 `-1 + errno` 不同。`*_np` 为 libc 扩展。接口语义见 [posix_spawn 手册](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)。

`run_process(argv, ProcessOptions)` 把超时、输出限制、stdin、cwd、环境变量、继承 FD 和取消/输出回调集中在一份配置中。默认超时 10 秒、保留输出上限 1 MiB。输出管道和 stdin socketpair 的端点使用 UniqueFd，spawn 的 actions/attributes 使用局部 RAII；配置或启动失败时自动释放资源。输出回调或 I/O 抛异常时，子进程清理器终止进程组并回收直接子进程。

选择 spawn，使任务启动配置集中在 action/attribute 中，适合嵌入多线程 SDK。fork 后在子进程里执行复杂 C++ 逻辑会涉及继承的锁和运行库状态；一般命令启动采用 spawn，专门的 namespace 引导保留 fork。

### fork 与 execv：Guest 专用引导

| 接口 | 用法与作用 |
|---|---|
| `fork` | 返回 0 的子进程执行 `unshare(CLONE_NEWUSER)`，父进程写映射并取得 namespace FD |
| `pause` | 引导子进程等待信号，使父进程有时间配置映射 |
| `_exit` | 子进程引导失败时立即结束，不执行继承的 C++ 析构和 stdio 刷新 |
| `execv` | Guest task 子模式完成降权后，以 `execv(argv[1], argv + 1)` 替换为任务程序 |
| `chdir` | libcrun 工作进程进入 bundle，再加载相对路径 config.json |
| `getpid` | 检查容器 agentd 为 PID 1，或取得任务 PID 写入 cgroup.procs |

exec 成功时不返回；返回意味着启动失败。Guest task 模式是新 spawn 子进程的入口，降权发生在该子进程中，服务身份保持不变。

### 等待、信号与进程组

| 接口 | 用法与作用 |
|---|---|
| `waitid` | `waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT)`，观察退出但暂不回收 |
| `waitpid` | 最终回收直接子进程；PID 1 用 `waitpid(-1, ..., WNOHANG)` 回收已退出的孤儿 |
| `kill` | `kill(-pid, SIGKILL)` 终止进程组；容器 PID 1 还逐个清理 namespace 内残留任务 |
| `WIFEXITED/WEXITSTATUS` | 从 wait 状态解析正常退出码 |
| `WIFSIGNALED/WTERMSIG` | 解析信号终止，项目返回 `128 + signal` |

WNOWAIT 保留子进程退出状态，在管道清理结束前不释放 PID。仅等待直接子进程不能覆盖后台后代；项目进一步使用进程组、容器 PID namespace 或 Guest cgroup 回收。

实现：[process.cpp](../src/lib/process.cpp)、[task_runner.cpp](../src/agentd/task_runner.cpp)、[container_bootstrap.cpp](../src/agentd/container_bootstrap.cpp)、[microvm_bootstrap.cpp](../src/agentd/microvm_bootstrap.cpp)。

## 3. 管道、socket 与等待

| 接口 | 调用方式 | 项目作用 |
|---|---|---|
| `pipe2` | `pipe2(fds, O_CLOEXEC)` | 收集 stdout/stderr；Guest idmap 父子进程传递准备结果 |
| `socketpair` | `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)` | 子进程 stdin；测试中的双端协议连接 |
| `socket` | `socket(AF_UNIX/AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0)` | 创建宿主 Unix 控制连接或 Guest vsock listener |
| `bind` | 把 sockaddr_un / sockaddr_vm 绑定到 socket | 设置 Unix 路径或 vsock CID/端口 |
| `listen` | `listen(fd, backlog)` | 进入接受连接状态 |
| `accept4` | `accept4(listener, address, size, SOCK_CLOEXEC)` | 接受连接并原子设置 CLOEXEC |
| `connect` | 非阻塞连接 Unix socket | Session 接入常驻 agentd |
| `getsockopt` | `SO_ERROR` / `SO_ACCEPTCONN` | 查询建连结果 / 检查传入 FD 为 listener |
| `send` | `send(fd, data, size, flags)` | 发送协议或 stdin 字节 |
| `shutdown` | `shutdown(fd, SHUT_RDWR)` | 中断 socket 双向通信，唤醒等待 |
| `poll` | 等待 `POLLIN` 或 `POLLOUT` | 同时收集输出、传入 stdin，处理取消和期限 |

Unix 地址用 `sockaddr_un.sun_path`；项目检查路径长度。vsock 地址用 `sockaddr_vm.svm_cid/svm_port`；Guest 只接待 Host CID 2。SOCK_STREAM 提供字节流，消息边界由 IPC 帧头定义。

非阻塞 connect 返回 EINPROGRESS 后，等待 POLLOUT，再读 SO_ERROR 判定成功。可写事件本身不能代替连接结果检查。

发送 socket 数据使用 MSG_NOSIGNAL，对端关闭时以错误返回，避免 SIGPIPE 改变宿主进程行为。stdin 使用 socketpair 正是为了能设置这一标志，不修改应用全局信号处理。喂入 stdin 还使用 MSG_DONTWAIT；全部数据发送后关闭父端，让任务读到 EOF。

poll 同时处理 stdout/stderr 和 stdin，避免“写满输入后才读输出”的互相阻塞。每轮读取和写入有次数上限，使持续输出的任务仍能检查期限。当前每个连接和命令涉及少量 FD，poll 比引入 epoll 事件循环更直接。

实现：[socket.cpp](../src/ipc/socket.cpp)、[io.cpp](../src/ipc/io.cpp)、[process.cpp](../src/lib/process.cpp)、[main.cpp](../src/agentd/main.cpp)。

## 4. memfd：一次性交付生命周期请求

`memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING)` 创建匿名内存文件。ContainerClient 写入 CBOR 请求，通过 FD 3 交给工作进程。

写完后用 `fcntl(fd, F_ADD_SEALS, seals)` 加入：

| seal | 作用 |
|---|---|
| `F_SEAL_WRITE` | 禁止修改内容 |
| `F_SEAL_GROW` | 禁止扩大文件 |
| `F_SEAL_SHRINK` | 禁止缩小文件 |
| `F_SEAL_SEAL` | 禁止再修改 seal 集合 |

工作进程先用 fstat 核对类型和大小，再 pread 完整请求并关闭控制 FD。请求没有文件路径，也不占用任务 stdin；交付后内容固定。

临时磁盘文件需要路径与清理，argv 不适合传递结构化二进制；memfd 正好匹配短进程的一次性请求。agentd 的持续消息使用 socket，详见 [Session 与 IPC](03-session-ipc.md)。

项目使用 memfd，但没有直接使用 mmap/munmap。

实现：[memory_file.cpp](../src/lib/memory_file.cpp)、[container_client.cpp](../src/virtualization/container_client.cpp)、[crun_worker.cpp](../src/virtualization/container/crun_worker.cpp)。

## 5. 文件访问、原子替换与锁

### 元数据与权限

| 接口 | 调用方式与作用 |
|---|---|
| `fstat` | 从已打开 FD 获取类型、大小、链接数；检查读取文件、写入目标和 memfd |
| `lstat` | 检查路径本身，不跟随最终符号链接；验证管理目录、工作文件和基准磁盘用量 |
| `statfs` | 查询文件系统类型；Guest bootstrap 检查根为 virtiofs 使用的 FUSE 文件系统 |
| `access` | `access(path, X_OK)`，启动前检查内部程序可执行性 |
| `lchown` | rootful 准备阶段设置 B 和运行数据的任务 UID/GID；不跟随最后的符号链接 |
| `fchmod` | 提交暂存文件前设置目标权限 |
| `getrandom` | 取得 16 字节随机值，生成暂存文件名，结合 O_EXCL 创建 |
| `flock` | `flock(fd, LOCK_EX | LOCK_NB)`，对单个 Sandbox 操作加跨进程建议锁 |

flock 锁覆盖 SDK 的执行、读写、预览和生命周期操作。它协调遵守锁协议的调用方，不是任务文件的强制锁；冻结运行时解决预览期间任务写入问题。

### openat2：在内核路径解析中指定边界

```cpp
open_how how{};
how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
int fd = syscall(SYS_openat2, workspace_fd, relative_path, &how, sizeof(how));
```

传入已打开的工作区目录 FD 和相对路径，三个 resolve 标志分别限制向上逃逸、符号链接解析和跨挂载。O_NOFOLLOW 只作用于最后一段，openat2 可约束整个解析过程，避免“先检查字符串或 canonical，再打开”的两步竞争。结构体零初始化，size 传 sizeof；当前通过 syscall 调用。参见 [openat2 手册](https://man7.org/linux/man-pages/man2/openat2.2.html)。

SDK 文件 API 先把工作区内绝对路径转换为相对路径。read 在打开后 fstat 核对普通文件与大小；write 通过 openat2 打开父目录，再通过 openat 检查目标和创建文件。

这些约束属于 read/write 文件 API。execute 中的程序按整个隔离环境的挂载和身份规则访问文件。

### 原子写入

项目顺序为：

```text
openat2 打开父目录
  → openat(O_PATH | O_NOFOLLOW) 检查已有目标
  → getrandom + openat(O_CREAT | O_EXCL) 创建同目录暂存文件
  → write 全部内容
  → fchmod 设置权限
  → fsync 暂存文件
  → renameat 替换目标
```

`renameat(parent_fd, temporary, parent_fd, target)` 在同一目录内原子替换目录项，读者看到旧文件或新文件。异常或上传中断时用 `unlinkat(parent_fd, temporary, 0)` 删除未提交文件。

`fsync(fd)` 同步文件内容与相关元数据；这里没有 fsync 父目录，因此原子可见性与掉电后目录项持久性是不同保证。VM 预览使用 `syncfs(workspace_fd)` 同步该文件系统，再暂停 VMM。

实现：[filesystem.cpp](../src/lib/filesystem.cpp)、[workspace_files.cpp](../src/agentd/workspace_files.cpp)、[sandbox.cpp](../src/sandbox.cpp)。

## 6. 身份、权限与进程限制

| 接口 | 项目用法 |
|---|---|
| `geteuid/getegid` | 判断 rootless，生成 UID/GID 单映射，校验管理目录归属 |
| `setgroups` | Guest task 用 `setgroups(0, nullptr)` 清空补充组 |
| `setgid/setuid` | Guest task 切换为 65534；先组后用户 |
| `setfsgid/setfsuid` | Guest agentd 文件操作临时使用任务文件身份，操作结束恢复 |
| `prctl(PR_SET_NO_NEW_PRIVS)` | 任务 exec 不通过 setuid 等机制获得新权限 |
| `prctl(PR_SET_DUMPABLE)` | 容器 agentd 设置为 0，限制任务对其 ptrace 和受权限检查的 proc 访问 |
| `setrlimit` | Guest task 设置 RLIMIT_NOFILE=256、RLIMIT_CORE=0 |

Guest agentd 保留配置挂载和 cgroup 的权限，任务在独立子进程中降权后 exec。WorkspaceFiles 用 FileIdentity RAII 临时设置调用线程的 fsuid/fsgid，并通过查询确认切换；setfsuid/setfsgid 返回旧值，不按通常的 -1/errno 模式报告设置失败。参见 [setfsuid 手册](https://man7.org/linux/man-pages/man2/setfsuid.2.html)。

进程身份降权、文件访问身份和 noNewPrivileges 分别约束不同操作，不能互相替代。rlimit 限制单进程资源，cgroup 覆盖任务集合。

实现：[task_runner.cpp](../src/agentd/task_runner.cpp)、[workspace_files.cpp](../src/agentd/workspace_files.cpp)、[container_bootstrap.cpp](../src/agentd/container_bootstrap.cpp)。

## 7. Namespace、挂载与 Guest 文件身份映射

`unshare(CLONE_NEWUSER)` 是源码中直接使用的 namespace 创建接口。容器的 pid、mount、network、ipc、uts、cgroup 和 user namespace 由 OCI 配置交给 libcrun 建立，项目没有直接调用 clone/setns/pivot_root。

### mount

Guest 使用 `mount(source, target, type, flags, data)`：

- 将 `sandbox-workspace` 和 `sandbox-data` virtiofs 挂到工作区与 runtime-data。
- 使用 MS_BIND 将 build/env/cache 映射到固定目录。
- 将 tmpfs 挂到 `/tmp`，设置 mode=1777、size=16m。
- 对共享目录使用 MS_NOSUID、MS_NODEV，限制 set-ID 和设备文件语义。

只读系统工具目录由宿主 OCI bind 配置和 libkrun 的只读 virtiofs root 提供；Guest 可写 B 使用独立共享。

### open_tree、mount_setattr、move_mount

这三个接口通过 syscall 调用，组合成 idmapped mount：

```text
fork → unshare(CLONE_NEWUSER)
  → 父进程写 uid_map/gid_map，取得 /proc/<pid>/ns/user FD
  → open_tree(AT_FDCWD, path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)
  → mount_setattr(tree_fd, "", AT_EMPTY_PATH, MOUNT_ATTR_IDMAP + userns_fd)
  → move_mount(tree_fd, "", AT_FDCWD, path, MOVE_MOUNT_F_EMPTY_PATH)
```

open_tree 克隆挂载句柄；mount_setattr 设置其用户映射；move_mount 将调整后的挂载附着到目标。项目配置 `0 65534 1` 映射，使 Guest 任务以 65534 使用共享文件，而宿主文件继续归调用用户。参见 [mount_setattr 手册](https://man7.org/linux/man-pages/man2/mount_setattr.2.html)。

直接 chown 会改变共享 B 的宿主所有权；统一让 Guest 任务用 root 则扩大任务权限。idmapped mount 将所有权视图调整限定在 Guest 挂载中。

实现：[microvm_bootstrap.cpp](../src/agentd/microvm_bootstrap.cpp)、[oci.cpp](../src/virtualization/environment/oci.cpp)。

## 8. cgroup v2 与 procfs：文件形式的内核接口

cgroup 配置通过 open/write 或 C++ 文件流完成，内核控制逻辑由文件内容触发。宿主限制由 libcrun/systemd 配置，SDK 再读取实际值核对；Guest 任务组由 bootstrap 直接创建。

| 文件 | 项目作用 |
|---|---|
| `cgroup.controllers` | 检查可用及已委派的控制器 |
| `cgroup.subtree_control` | Guest 写入 `+pids`，启用子层级控制器 |
| `cgroup.procs` | Guest 任务在降权前写入自身 PID，后代继承所属组 |
| `memory.max` / `memory.swap.max` | 核对宿主内存预算及禁止 swap |
| `cpu.max` | 核对 quota/period |
| `pids.max` | 限制任务数量；内核按 task 计数，线程也占用额度 |
| `cgroup.freeze` | 写入 1/0 冻结/恢复 Guest 任务 |
| `cgroup.events` | 等待 frozen=1/0 或 populated=0 的实际状态 |
| `cgroup.kill` | 写入 1 回收任务组及后代 |

freeze 写入和冻结完成是两个阶段，项目读取 events 确认完成后再 syncfs。cgroup 回收按成员关系覆盖进程，能处理 setsid 脱离进程组的后代。语义见 [内核 cgroup v2 文档](https://docs.kernel.org/admin-guide/cgroup-v2.html)。

procfs 用于：读取 `/proc/<pid>/cgroup` 定位实际组；枚举容器 `/proc` 清理任务；写 uid_map/gid_map 设置映射；打开 `/proc/<pid>/ns/user` 取得 namespace 句柄。这些是内核提供的文件接口，不是普通配置文件。

实现：[resources.cpp](../src/virtualization/container/resources.cpp)、[microvm_bootstrap.cpp](../src/agentd/microvm_bootstrap.cpp)、[container_bootstrap.cpp](../src/agentd/container_bootstrap.cpp)。

## 9. KVM 与运行时库接口

SDK 打开 `/dev/kvm`，用 `ioctl(fd, KVM_GET_API_VERSION, 0)` 核对 API 版本为 12，随后关闭检查 FD。VM 内存、vCPU 和运行循环由 libkrun 实现，项目没有直接操作 KVM_CREATE_VM/KVM_RUN。

| 库 API | 项目作用 |
|---|---|
| `libcrun_container_load_from_file/run/free` | 加载 OCI config，启动容器并释放配置对象 |
| `libcrun_container_state` | 输出容器状态 |
| `libcrun_container_pause/unpause` | 暂停和恢复 |
| `libcrun_container_killall/delete` | 终止和删除容器 |
| `libcrun_close_inherited_fds` | 按 preserve_fds 处理启动 FD |
| `libcrun_init_logging`、handler manager、error release | 初始化运行时上下文并管理错误与资源 |
| `krun_create_ctx` | 创建 VMM 上下文 |
| `krun_set_vm_config` | 配置 vCPU 与 RAM |
| `krun_disable_implicit_vsock` | 关闭隐式 vsock/TSI 代理 |
| `krun_add_vsock` / `krun_add_vsock_port2` | 添加控制设备和 Host Unix/Guest vsock 映射 |
| `krun_add_virtiofs4` | 提供只读 root、可写 B 和 runtime-data |
| `krun_set_workdir/set_console_output/set_exec` | 配置 Guest cwd、console 文件和 agentd 启动参数 |
| `krun_start_enter` | 进入 VM 运行 |

这些是库函数，不是系统调用。libgit2 的对象、提交、index 和 diff API 属于 Workspace 业务实现，见 [Git Workspace](04-workspace.md)。

实现：[crun_ops.c](../src/virtualization/container/crun_ops.c)、[krun_runtime.cpp](../src/virtualization/microvm/krun_runtime.cpp)、[krun_runner.cpp](../src/virtualization/microvm/krun_runner.cpp)。

## 10. libc、C++ 与测试辅助接口

| 接口 | 作用与用法 |
|---|---|
| `getpwuid` | 按有效 UID 获取宿主账户 home，确定管理目录和 systemd 工作进程环境 |
| `getenv/setenv` | 读取可信宿主资源配置，设置工作进程的环境；任务环境通过 posix_spawn 的 envp 传入 |
| `errno/strerror/perror` | 取得系统调用失败原因，构造异常或写入 stderr |
| `mkdtemp` | 样例和测试以含 XXXXXX 的可写字符数组创建独占临时目录 |
| `std::filesystem` | 创建、删除、重命名、遍历、复制、权限和符号链接操作；标准库内部转换为平台操作 |
| `std::ifstream/ofstream` | JSON、Git 元数据和内核伪文件的读取与写入 |
| `std::thread`、mutex、scoped_lock、atomic | agentd 执行与取消并发、Session 状态保护、协议写入串行化 |
| `steady_clock`、`sleep_for` | 单调期限与轮询间隔，不依赖可调整的墙上时间 |

项目通过 C++ 标准库使用线程与时间接口，没有直接调用 pthread_create/clock_gettime/nanosleep。测试复用 socketpair、spawn、kill 和 waitpid 构造协议服务；基准以 lstat 的 st_size/st_blocks 区分逻辑大小与分配磁盘量。

## 阅读顺序

从 [process.cpp](../src/lib/process.cpp) 看 FD、进程和非阻塞监督，再看 [socket.cpp](../src/ipc/socket.cpp) 与 [Session](03-session-ipc.md)。文件边界看 [workspace_files.cpp](../src/agentd/workspace_files.cpp)，身份与隔离看 [microvm_bootstrap.cpp](../src/agentd/microvm_bootstrap.cpp)。
