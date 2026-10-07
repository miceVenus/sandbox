# Git Workspace

A 是宿主源仓库，B 是任务独立仓库。Workspace 使用 libgit2 创建 B 和预览改动；后端负责让 B 出现在隔离环境内。

## 目录与创建

```text
<管理目录>/<sandbox-id>/
  sandbox.json
  workspace/
    files/          # B，内含任务自己的 .git
    manager.git/    # 宿主私有对象库、基线和预览 index
    workspace.txt   # 来源与基线记录
```

创建时解析源仓库和 revision，检查源工作树干净且提交存在，记录基线、源 HEAD 和分支。选定提交的可达历史先导入 manager.git，再导入 B；B checkout 到 `sandbox` 分支，没有 remote。

只带入该提交及其历史。源仓库其他分支、未提交内容、未跟踪文件和忽略文件不复制。当前拒绝 submodule。

隔离环境只共享 files。任务可使用 Git 提交，也可修改自己的 `.git`；manager.git 和管理记录留在宿主。

## 为什么采用独立仓库

| 方法 | 收益 | 本项目的取舍 |
|---|---|---|
| 独立 Git 仓库 | 独立元数据、提交历史、差异工具 | 当前实现，任务与源仓库引用分离 |
| `git worktree` | 复用对象库，创建成本低 | 共享管理元数据，任务可写 Git 信息的边界更复杂 |
| 普通文件复制 | 实现直接 | 还需建立基线、提交与结果比较机制 |
| OverlayFS / 文件系统 COW | 减少文件复制 | 需处理挂载、删除标记和快照，仍需单独实现 Git 结果语义 |

独立仓库让 B 的 Git 操作与 A 分离，同时直接获得版本和补丁语义。当前对象导入和 checkout 会占用独立存储，没有文件系统 COW。

libgit2 在 SDK 内提供对象、树、index 和 diff API，避免 Git 子进程与文本结果解析。Git CLI 更适合直接复用完整 Git 命令，但本模块的操作固定，库接口便于 RAII 和结构化错误处理。代价是 Git 计算占用 SDK 进程资源，操作期限和内存配额需要单独设计。

## 改动预览

`get_changes()` 先暂停运行时并同步 B，再调用 Workspace 的 status/diff，最后恢复运行。容器直接挂载 B；microVM 先冻结 Guest 任务并 syncfs，再冻结 VMM。

预览打开 manager.git，把 B files 设为工作树。diff 刷新私有 index，以基线树比较当前快照，包含提交后的修改、未提交修改、新增、删除和二进制内容；忽略文件不导出。status 与 diff 输出各有 1 MiB 上限。diff 会更新私有 index，status 的两列反映该 index 与当前文件的关系。

直接读取 B 的 `git diff` 会受 B 的 HEAD、index 和配置影响，任务提交后差异也可能消失。私有基线让“相对创建时改了什么”独立于任务 Git 历史。宿主操作使用隔离配置，任务 `.git` 不作为预览依据。

## 文件边界与交付

预览检查文件类型，拒绝特殊文件、硬链接和嵌套 `.git`；顶层 B `.git` 不遍历。符号链接可作为 Git 文件记录，但文件 API 不跟随它们。

源仓库当前只读用于创建。结果交付尚未实现：下一步需要冻结最终快照、固定受控提交、处理 A 的并发变化和合并冲突。普通目录来源及快照导出也列入 [TODO](07-todo.md)。

独立接口为 `Workspace::create/open/status/diff`，以及路径、基线和来源访问器。`source_head/source_branch` 在 create 返回对象中填充，Workspace::open 当前只恢复来源路径和基线；Sandbox 的源 HEAD、分支由 sandbox.json 恢复。

实现：[workspace.cpp](../src/workspace/workspace.cpp)、[git_ops.cpp](../src/workspace/git_ops.cpp)、[workspace_files.cpp](../src/workspace/workspace_files.cpp)。
