#!/usr/bin/env python3
"""Build a root-owned, minimal learning bundle. Prints its absolute path.

This is not a production policy: no user namespace, seccomp or workspace quota.
Run with sudo; every invocation creates a fresh directory under /tmp.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

if os.geteuid() != 0:
    raise SystemExit("run: sudo python3 prepare_demo.py")

crun = "/usr/local/bin/crun"
busybox = "/usr/bin/busybox"

description = subprocess.check_output(["/usr/bin/file", busybox], text=True)
if "statically linked" not in description:
    raise SystemExit("this demo requires a statically linked /usr/bin/busybox")

bundle = Path(tempfile.mkdtemp(prefix="crun-cpp-demo-"))
rootfs = bundle / "rootfs"
for directory in ("bin", "proc", "dev", "tmp", "workspace"):
    (rootfs / directory).mkdir(parents=True, exist_ok=True)
shutil.copy2(busybox, rootfs / "bin/busybox")
for applet in subprocess.check_output([busybox, "--list"], text=True).splitlines():
    if applet != "busybox":
        (rootfs / "bin" / applet).symlink_to("busybox")

workspace = bundle / "workspace"
workspace.mkdir(mode=0o700)
os.chown(workspace, 65534, 65534)
subprocess.run([crun, "spec"], cwd=bundle, check=True, capture_output=True)
path = bundle / "config.json"
config = json.loads(path.read_text())
config["hostname"] = "agent-demo"
config["root"] = {"path": "rootfs", "readonly": True}
process = config["process"]
process.update({
    "terminal": False,
    "user": {"uid": 65534, "gid": 65534, "additionalGids": []},
    "args": ["/bin/sh", "-c", "while :; do /bin/sleep 3600; done"],
    "cwd": "/workspace",
    "env": ["PATH=/bin", "HOME=/workspace", "LANG=C"],
    "noNewPrivileges": True,
    "capabilities": {key: [] for key in (
        "bounding", "effective", "permitted", "inheritable", "ambient")},
    "rlimits": [
        {"type": "RLIMIT_NOFILE", "soft": 256, "hard": 256},
        {"type": "RLIMIT_CORE", "soft": 0, "hard": 0},
    ],
})
# Preserve the default /proc and /dev protections, omit sysfs/cgroup mounts.
config["mounts"] = [m for m in config["mounts"]
                    if m["destination"] == "/proc" or m["destination"].startswith("/dev")]
config["mounts"] += [
    {"destination": "/tmp", "type": "tmpfs", "source": "tmpfs",
     "options": ["nosuid", "nodev", "noexec", "mode=1777", "size=16m"]},
    {"destination": "/workspace", "type": "bind", "source": str(workspace),
     "options": ["bind", "rw", "nosuid", "nodev", "private"]},
]
linux = config["linux"]
linux["cgroupsPath"] = "/" + bundle.name
resources = linux.setdefault("resources", {})
resources.update({
    "memory": {"limit": 256 * 1024 * 1024, "swap": 256 * 1024 * 1024},
    "cpu": {"period": 100000, "quota": 100000},
    "pids": {"limit": 64},
})
path.write_text(json.dumps(config, indent=2) + "\n")
print(bundle)
