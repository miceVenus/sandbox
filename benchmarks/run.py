#!/usr/bin/env python3
"""Run real SDK VMs serially; keep raw samples and a Chinese Markdown report."""
import argparse
import csv
import datetime
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile

MIB = 1024 * 1024


def command(args):
    return subprocess.check_output(args, text=True).strip()


def disk(path):
    paths = [path]
    for base, dirs, files in os.walk(path, followlinks=False):
        paths.extend(Path(base) / name for name in dirs + files)
    stats = [p.lstat() for p in paths]
    return {"logical_bytes": sum(s.st_size for s in stats),
            "allocated_bytes": sum(s.st_blocks * 512 for s in stats)}


def fixture(path, mib):
    path.mkdir()
    rng = random.Random(20261003)
    # Incompressible tracked data makes Git/object and working-file costs visible.
    for n in range(128):
        (path / f"data-{n:03}.bin").write_bytes(rng.randbytes(mib * MIB // 128))
    (path / "main.cpp").write_text(
        '#include <algorithm>\n#include <iostream>\n#include <numeric>\n'
        '#include <vector>\nint main() { std::vector<int> v(10000); '
        'std::iota(v.begin(), v.end(), 0); std::sort(v.rbegin(), v.rend()); '
        'std::cout << std::accumulate(v.begin(), v.end(), 0LL) << "\\n"; }\n')
    subprocess.run(["git", "-C", str(path), "init", "-q"], check=True)
    subprocess.run(["git", "-C", str(path), "add", "."], check=True)
    subprocess.run(["git", "-C", str(path), "-c", "user.name=Benchmark",
                    "-c", "user.email=benchmark@example.invalid", "commit", "-qm", "fixture"], check=True)


def summary(values):
    values = sorted(values)
    return {"n": len(values), "median": statistics.median(values),
            "p95": values[math.ceil(.95 * len(values)) - 1],
            "min": min(values), "max": max(values)}


def aggregate(data):
    runs = data["runs"]
    warm = runs[1:]
    result = {"mode": data["mode"], "host_budget_mib": data["host_budget_mib"],
              "guest_ram_mib": data["guest_ram_mib"], "samples": len(runs),
              "first_create_ms": runs[0]["create_ms"], "warm_create_ms": summary([r["create_ms"] for r in warm])}
    for key in ("sdk_overhead_ms", "prepare_ms", "runtime_start_ms", "stop_ms", "destroy_ms"):
        result[key] = summary([r[key] for r in warm])
    for key in ("cgroup_current_bytes", "cgroup_anon_bytes", "cgroup_file_bytes", "cgroup_kernel_bytes",
                "VmRSS_bytes", "Rss_bytes", "Pss_bytes"):
        values = [r["memory_idle"][key] for r in warm if key in r["memory_idle"]]
        if values:
            result["idle_" + key] = summary(values)
    result["true_exec_ms"] = summary([ms for r in warm for ms in r["true_exec_ms"][1:]])
    result["disk_ready_bytes"] = summary([r["disk_ready"]["total"]["allocated_bytes"] for r in warm])
    result["disk_logical_bytes"] = summary([r["disk_ready"]["total"]["logical_bytes"] for r in warm])
    for part in ("guest", "vmm", "workspace", "runtime-data"):
        result["disk_" + part + "_bytes"] = summary([r["disk_ready"][part]["allocated_bytes"] for r in warm])
    result["lifetime_cgroup_peak_bytes"] = summary([r["memory_after_work"]["cgroup_peak_bytes"] for r in warm])
    result["compile_successes"] = sum(r.get("compile_result", {}).get("exit_status") == 0 for r in runs)
    if "compile_ms" in runs[0]:
        result["compile_ms"] = summary([r["compile_ms"] for r in warm])
        result["compile_peak_bytes"] = summary([r["compile_memory"]["sampled_cgroup_peak_bytes"] for r in warm])
        result["build_disk_delta_bytes"] = summary([
            r["disk_after_work"]["total"]["allocated_bytes"] - r["disk_ready"]["total"]["allocated_bytes"] for r in warm])
    result["cleanup_all_passed"] = all(all(r["cleanup"].values()) for r in runs)
    return result


def report(out, env, summaries):
    lines = ["# libkrun SDK 性能实测", "", f"测量时间：{env['started_at']}。构建：{env['build_type']}。", "",
             "## 环境与口径", "", f"- 系统：{env['os']}；内核 {env['kernel']}；CPU {env['cpu']}（{env['logical_cpus']} 个逻辑 CPU）。",
             f"- 宿主内存：{env['host_ram_bytes']/MIB:.0f} MiB；测试前 MemAvailable {env['available_bytes']/MIB:.0f} MiB。桌面同时在运行其他程序，结果可能受其影响。",
             "- 普通用户/rootless，真实 KVM + libkrun VM；1 vCPU，网络关闭。宿主仅委派 memory/pids，CPU quota 显式设为 0。",
             f"- 固定随机种子的 Git A：{env['workspace_payload_mib']} MiB 数据、128 个数据文件和 1 个 C++ 文件；A 实际分配 {env['source_disk']['allocated_bytes']/MIB:.2f} MiB（含 Git 对象）。",
             "- 每组串行运行，首个样本单独展示，其余作为重复启动样本。保留宿主页缓存；首个样本也不代表严格冷启动。P95 使用 nearest-rank，小样本 P95 接近/等于最大值。",
             "- create 时间从 SDK create 调用到返回，包含 Git B 快照、Guest/VMM 文件准备、启动和 Guest ready/资源验证；不包含 fixture 构建、Manager 构造或后续监测。start 阶段包含保护容器启动，不是纯内核启动。sdk_overhead_ms 为总 create 减去 prepare/start，包含 Git 快照、校验与元数据写入。",
             "- 内存空闲值：create 返回后等待 1 秒，从 Host VMM cgroup 和 /proc 读取。cgroup 含匿名页、文件页缓存和内核开销，memory.peak 是该 VM 生命周期内的内核统计峰值。RSS/PSS 为 VMM 进程视角，两者与 cgroup 不可相加。",
             "- 编译阶段每 10 ms 采样 memory.current，因此阶段峰值只是采样下界；生命周期 memory.peak 是内核维护的峰值。Guest meminfo 保存于原始 JSON，不能再加到 VMM RSS 上。",
             "- 磁盘实际分配为 lstat.st_blocks × 512，含目录、Git 和运行环境，无符号链接跟随。统计 Host 会话目录，HostTools 只读共享的 /usr 等目录没有复制，故不计入每会话新增占用。", "",
             "## 启动、空闲内存与磁盘", "",
             "| 环境 | Host 上限 / Guest RAM MiB | 样本总数 | 首次 create ms | 重复 create 中位 / P95 ms | 空闲 cgroup MiB | 空闲 VMM RSS MiB | 空闲 VMM PSS MiB | 新会话磁盘 MiB |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    def median(s, key, scale=1):
        return f"{s[key]['median']/scale:.2f}" if key in s else "不可读取"
    for s in summaries:
        lines.append(f"| {s['mode']} | {s['host_budget_mib']} / {s['guest_ram_mib']} | {s['samples']} | {s['first_create_ms']:.1f} | {s['warm_create_ms']['median']:.1f} / {s['warm_create_ms']['p95']:.1f} | {median(s, 'idle_cgroup_current_bytes', MIB)} | {median(s, 'idle_VmRSS_bytes', MIB)} | {median(s, 'idle_Pss_bytes', MIB)} | {median(s, 'disk_ready_bytes', MIB)} |")
    lines += ["", "## 阶段与磁盘拆分（重复样本中位数）", "",
              "| 环境 / Host MiB | SDK 其余耗时 ms | 环境准备 ms | Runtime start ms | Guest 文件 MiB | VMM 文件 MiB | B/Git MiB | 可写运行数据 MiB |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for s in summaries:
        lines.append(f"| {s['mode']} / {s['host_budget_mib']} | {median(s, 'sdk_overhead_ms')} | {median(s, 'prepare_ms')} | {median(s, 'runtime_start_ms')} | {median(s, 'disk_guest_bytes', MIB)} | {median(s, 'disk_vmm_bytes', MIB)} | {median(s, 'disk_workspace_bytes', MIB)} | {median(s, 'disk_runtime-data_bytes', MIB)} |")
    lines += ["", "## 命令与编译", "", "`/bin/true` 每个 VM 调用 6 次，统计重复启动 VM 中后 5 次的 SDK exec 总耗时，包含状态检查、RPC、Guest 执行和任务回收。", "",
              "编译任务为 `g++ -O2 /workspace/main.cpp -o /build/app`，程序使用 vector、algorithm、numeric、iostream，运行结果应为 49995000。成功只代表这个小任务，不能推广为大型项目编译需求。", "",
              "| 环境 / Host MiB | exec 中位 / P95 ms | 编译成功 / 总次数 | 编译中位 ms | 编译采样峰值 MiB | 生命周期内存峰值 MiB | 编译新增磁盘 MiB |", "|---|---:|---:|---:|---:|---:|---:|"]
    for s in summaries:
        compile_count = f"{s['compile_successes']} / {s['samples']}" if "compile_ms" in s else "未测（无编译器）"
        lines.append(f"| {s['mode']} / {s['host_budget_mib']} | {s['true_exec_ms']['median']:.2f} / {s['true_exec_ms']['p95']:.2f} | {compile_count} | {median(s, 'compile_ms')} | {median(s, 'compile_peak_bytes', MIB)} | {median(s, 'lifetime_cgroup_peak_bytes', MIB)} | {median(s, 'build_disk_delta_bytes', MIB)} |")
    lines += ["", "## 回收", "", "stop 保留 B 和准备文件；destroy 删除整个沙箱。每次确认沙箱目录、crun runtime 状态目录和 VMM cgroup 都已消失。", "",
              "| 环境 / Host MiB | stop 中位 ms | destroy 中位 ms | 全部回收检查 |", "|---|---:|---:|---|"]
    for s in summaries:
        lines.append(f"| {s['mode']} / {s['host_budget_mib']} | {median(s, 'stop_ms')} | {median(s, 'destroy_ms')} | {'通过' if s['cleanup_all_passed'] else '失败'} |")
    lines += ["", "## 共享与测量限制", "",
              f"本次构建 libexec 工具目录实际分配 {env['sdk_libexec_disk']['allocated_bytes']/MIB:.2f} MiB；这是一次性的共享构建资源，已从每会话目录统计中分离。静态 SDK archive 和编译输出不属于运行中的会话占用。",
              "当前实现仍逐会话复制 Guest agent、动态依赖、VMM 和 libkrunfw。磁盘准备可能成为启动成本，不能据此认为 libkrun 内核本身启动慢。该测试没有清空缓存、关闭 swap 或更改宿主调度配置，也没有测并发多 VM、长时间任务、pip 安装或大项目编译。",
              "Host 上限和 Guest RAM 是容量配置，不是已使用物理内存；空闲 Guest 不会触及全部 RAM。cgroup 文件缓存计费受缓存首次归属影响，也不等于整台宿主机内存增量。这里统计的是 VMM cgroup/进程，未覆盖宿主 Manager、Git worker 等短时准备进程的峰值。",
              "权限不允许读取 smaps_rollup 时 PSS 标为不可读取，不用 RSS 冒充 PSS。峰值和磁盘原始数据、memory.events/OOM、Guest meminfo、命令状态及回收检查均在对应 JSON 中。", "",
              "复现命令见 ../README.md。原始环境见 environment.json，聚合数据见 summary.json；不设性能阈值，也未把基准加入 CTest。", ""]
    (out / "report.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build/benchmark-release")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=11, help="1 first observed + 10 repeated")
    parser.add_argument("--workspace-mib", type=int, default=1)
    parser.add_argument("--modes", nargs="+", choices=("minimal", "host-tools"), default=["minimal", "host-tools"])
    parser.add_argument("--host-mib", nargs="+", type=int, choices=(256, 512, 1024), default=[256, 512])
    args = parser.parse_args()
    if not 2 <= args.samples <= 1000 or not 1 <= args.workspace_mib <= 256:
        parser.error("samples 2..1000, workspace-mib 1..256")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = args.build_dir.resolve()
    binary = build / "sandbox-benchmark"
    if not binary.is_file():
        parser.error(f"build benchmark first: {binary}")
    meminfo = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    cpu = next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name"))
    cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines() if "=" in line and not line.startswith(("#", "//")))
    env = {"started_at": datetime.datetime.now().astimezone().isoformat(),
           "os": command(["lsb_release", "-ds"]), "kernel": platform.release(), "cpu": cpu,
           "logical_cpus": os.cpu_count(), "host_ram_bytes": int(meminfo["MemTotal"].split()[0]) * 1024,
           "available_bytes": int(meminfo["MemAvailable"].split()[0]) * 1024,
           "host_swap_used_bytes": (int(meminfo["SwapTotal"].split()[0]) - int(meminfo["SwapFree"].split()[0])) * 1024,
           "build_type": cache.get("CMAKE_BUILD_TYPE:STRING", "unknown"),
           "build_dir": str(build), "uid": os.getuid(), "workspace_payload_mib": args.workspace_mib,
           "compiler": command(["c++", "--version"]).splitlines()[0],
           "benchmark_configuration": {"samples": args.samples, "modes": args.modes, "host_mib": args.host_mib},
           "sdk_libexec_disk": disk(build / "libexec/bbm-sandbox"),
           "dependencies": {key: cache.get(key) for key in ("LIBKRUN_ROOT:PATH", "LIBKRUN_LIBRARY:FILEPATH", "LIBKRUNFW_LIBRARY:FILEPATH", "LIBCRUN_LIBRARY:FILEPATH")},
           "tmp_filesystem": command(["findmnt", "-T", "/tmp", "-n", "-o", "FSTYPE,SOURCE"])}
    env["runtime_libraries"] = {}
    for key in ("LIBKRUN_LIBRARY:FILEPATH", "LIBKRUNFW_LIBRARY:FILEPATH"):
        library = Path(cache[key]).resolve()
        env["runtime_libraries"][key] = {"file": str(library), "bytes": library.stat().st_size}
    root = Path(tempfile.mkdtemp(prefix="krun-bench-", dir="/tmp"))
    ok = False
    try:
        source = root / "A"
        fixture(source, args.workspace_mib)
        before = command(["git", "-C", str(source), "rev-parse", "HEAD"])
        env["source_disk"] = disk(source)
        (out / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
        summaries = []
        for mode in args.modes:
            for budget in args.host_mib:
                filename = out / f"{mode}-{budget}.json"
                subprocess.run([str(binary), str(source), str(root / "state"), mode,
                                str(budget), str(args.samples), str(filename)], check=True)
                data = json.loads(filename.read_text())
                summaries.append(aggregate(data))
        if command(["git", "-C", str(source), "status", "--porcelain"]) or command(["git", "-C", str(source), "rev-parse", "HEAD"]) != before:
            raise RuntimeError("source A changed")
        env["source_A_unchanged"] = True
        env["completed_at"] = datetime.datetime.now().astimezone().isoformat()
        (out / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
        (out / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
        with (out / "samples.csv").open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["mode", "host_mib", "index", "create_ms", "sdk_overhead_ms", "prepare_ms", "start_ms", "idle_cgroup_bytes", "disk_allocated_bytes"])
            for s in summaries:
                data = json.loads((out / f"{s['mode']}-{s['host_budget_mib']}.json").read_text())
                for r in data["runs"]:
                    writer.writerow([s["mode"], s["host_budget_mib"], r["index"], r["create_ms"], r["sdk_overhead_ms"], r["prepare_ms"], r["runtime_start_ms"], r["memory_idle"]["cgroup_current_bytes"], r["disk_ready"]["total"]["allocated_bytes"]])
        report(out, env, summaries)
        print(f"Report: {out / 'report.md'}", flush=True)
        ok = True
    finally:
        # Failed runs retain artifacts for inspection, never unlink a potentially live VM.
        if ok:
            shutil.rmtree(root)
        else:
            print(f"Benchmark incomplete; artifacts retained at {root}", flush=True)


if __name__ == "__main__":
    main()
