# SDK 性能基准

使用真实 libkrun VM，以普通用户串行运行。默认每组 11 次（首个观察样本 + 10 次重复启动），测试 Minimal/HostTools 与 256/512 MiB Host 内存上限的四个组合。

构建：

基准需要开发树资源，使用 Debug 预设并切换为 Release 优化：

```bash
cmake --preset debug -DCMAKE_BUILD_TYPE=Release -DSANDBOX_BUILD_BENCHMARKS=ON
cmake --build build/debug --target sandbox-benchmark -j 4
```

标准 `release` 预设面向安装包；这个性能构建使用开发树资源，无需安装。基准目标按需显式构建，普通 SDK 构建不编译它。测完后执行 `cmake --preset debug` 可恢复 Debug 配置。

运行（在项目根目录）：

```bash
python3 benchmarks/run.py --build-dir build/debug --output benchmarks/results/local
```

可以用 `--samples 21` 增加样本，`--workspace-mib 16` 测更大的 Git 工作区，`--build-dir build/debug` 指定构建目录。Python 需 3.9+，fixture 初始化需要 Git；被测 SDK 使用其 libgit2/libcrun/libkrun 后端。

单独测大工作区的 HostTools：

```bash
python3 benchmarks/run.py --build-dir build/debug --workspace-mib 16 --modes host-tools --host-mib 512 \
  --output benchmarks/results/large-workspace
```

比较 Debug 性能时，执行 `cmake --preset debug -DSANDBOX_BUILD_BENCHMARKS=ON`，重新构建基准即可。历史实测仍保留原来的 build-dir 和测量口径。

结果包括每组原始 JSON、samples.csv、summary.json、environment.json 和中文 report.md。时间单位为 ms，字节使用整数；报告中的 MiB = 1024² bytes。测试不重置宿主缓存，因此不声称测得冷启动。内存不是预设 RAM 数值：实际从 VMM 进程与 cgroup 读取。磁盘不跟随符号链接，不把共享 /usr 工具目录计为会话新增占用。

启动时间覆盖 `create` 完整调用，并额外拆出 Git B、环境准备、Runtime start；Runtime start 包含 Guest ready 和资源验证。每次运行确认 destroy 后 B、runtime 状态及 cgroup 全部消失，结束后确认 A 的 HEAD 和工作区未被更改。

当前脚本显式关闭 CPU quota，因为本机未委派 cpu controller。每 VM 仍配置 1 vCPU，内存和 PID 限制由 SDK 验证。编译任务只是一个小 C++ 程序，不能代表大型构建。失败不当作性能离群点丢弃：失败时保留已完成的 JSON 和临时路径，避免误删尚在运行的 VM。

基准是手动工具，不安装为 SDK CLI，也不加入 CTest 或设置性能断言。
