"""Verify the SDK-only installation using a separately built consumer and libcrun."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).resolve()
cmake = sys.argv[2]
json_package = sys.argv[3]
git_package = sys.argv[4]
krun_enabled = len(sys.argv) > 5 and sys.argv[5] == 'ON'
krun_root = sys.argv[6] if len(sys.argv) > 6 else ''
crun_root = sys.argv[7]
deps_prefix = sys.argv[8]


def run(args, expected=0, env=None):
    result = subprocess.run(args, capture_output=True, text=True, timeout=120, env=env)
    assert result.returncode == expected, (args, result.stdout[-4000:], result.stderr[-4000:])
    return result


with tempfile.TemporaryDirectory(prefix='sandbox-sdk-install-') as temporary:
    root = pathlib.Path(temporary)
    build = root / 'sdk-build'
    prefix = root / 'sdk install'
    # Reuse the dependency package, but configure a fresh installation build.
    run([cmake, '-S', str(source), '-B', str(build), '-DCMAKE_TOOLCHAIN_FILE=',
         f'-Dnlohmann_json_DIR={json_package}', f'-Dlibgit2_DIR={git_package}', '-DSANDBOX_DEVELOPMENT_BUILD=OFF',
         '-DBUILD_TESTING=OFF', f'-DCMAKE_INSTALL_PREFIX={prefix}',
         f'-DSANDBOX_ENABLE_LIBKRUN={"ON" if krun_enabled else "OFF"}',
         f'-DLIBKRUN_ROOT={krun_root}', f'-DLIBCRUN_ROOT={crun_root}',
         f'-DSANDBOX_DEPS_PREFIX={deps_prefix}',
         '-DCMAKE_INSTALL_LIBDIR=lib'])
    run([cmake, '--build', str(build), '-j2'])
    run([cmake, '--install', str(build)])
    assert not (prefix / 'bin/sandboxctl').exists()
    assert not (prefix / 'include/bbm-sandbox/client.hpp').exists()
    assert not (prefix / 'include/bbm-sandbox/crun_worker_client.hpp').exists()
    assert not (prefix / 'include/bbm-sandbox/virtualization/container/container_client.hpp').exists()
    for header in ['virtualization/container_client.hpp', 'virtualization/session.hpp',
                   'agentd/service.hpp', 'virtualization/container/crun_ops.h']:
        assert not (prefix / 'include/bbm-sandbox' / header).exists()
    assert not (prefix / 'include/bbm-sandbox/workspace/workspace_backend.hpp').exists()
    assert (prefix / 'include/bbm-sandbox/workspace/workspace.hpp').is_file()
    helper = prefix / 'libexec/bbm-sandbox/agentd'
    assert not (prefix / 'libexec/bbm-sandbox/sandbox-io').exists()
    assert not (prefix / 'libexec/bbm-sandbox/sandbox-git').exists()
    assert helper.is_file()
    runner = prefix / 'libexec/bbm-sandbox/sandbox-crun'
    assert runner.is_file()
    assert (prefix / 'libexec/bbm-sandbox/agentd').is_file()
    for header in ['virtualization/agentd_client.hpp', 'ipc/transport.hpp', 'ipc/protocol.hpp']:
        assert (prefix / 'include/bbm-sandbox' / header).is_file()
    assert not (prefix / 'libexec/bbm-sandbox/sandbox-task').exists()
    assert not (prefix / 'libexec/bbm-sandbox/sandbox-agentd').exists()

    caller = root / 'separate application'
    caller.mkdir()
    shutil.copyfile(source / 'tests/sdk_consumer.cpp', caller / 'main.cpp')
    (caller / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(sdk_consumer LANGUAGES CXX)
find_package(bbm-sandbox CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE bbm::sandbox_core)
''')
    caller_build = caller / 'build'
    run([cmake, '-S', str(caller), '-B', str(caller_build),
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-Dnlohmann_json_DIR={json_package}',
         f'-Dlibgit2_DIR={git_package}'])
    run([cmake, '--build', str(caller_build), '-j2'])
    consumer = caller_build / 'consumer'
    # A similarly named executable beside the caller must never be selected.
    decoy = caller_build / 'agentd'
    decoy.write_text('#!/bin/sh\necho WRONG_HELPER >&2\nexit 99\n')
    decoy.chmod(0o755)

    repo = root / 'source'
    repo.mkdir()
    run(['/usr/bin/git', 'init', '-q', str(repo)])
    (repo / 'a').write_text('original\n')
    run(['/usr/bin/git', '-C', str(repo), 'add', '.'])
    run(['/usr/bin/git', '-C', str(repo), '-c', 'user.name=Test', '-c',
         'user.email=test@example.invalid', 'commit', '-qm', 'baseline'])
    manager = root / 'manager'
    assert run([str(consumer), str(manager), str(repo)]).stdout == 'installed SDK passed\n'
    assert (repo / 'a').read_text() == 'original\n'
    records = list(manager.glob('*/sandbox.json'))
    assert len(records) == 1
    record = json.loads(records[0].read_text())
    assert record['state'] == 7  # Stopped.
    assert record['runtime_backend'] == 'oci-crun' and 'workspace_backend' not in record
    copied = records[0].parent / 'bundle/rootfs/sandbox-tools/agentd'
    assert copied.read_bytes() == helper.read_bytes()
    assert not (manager / 'runtime' / record['container_id']).exists()
    if krun_enabled:
        assert (prefix / 'libexec/bbm-sandbox/sandbox-krun').is_file()
        vm_manager = root / 'vm-manager'
        assert run([str(consumer), str(vm_manager), str(repo), 'vm']).stdout == 'installed SDK passed\n'
        vm_record = json.loads(next(vm_manager.glob('*/sandbox.json')).read_text())
        assert vm_record['runtime_backend'] == 'vm-libkrun'
        assert vm_record['resource_limits_verified'] and vm_record['state'] == 7
        assert (repo / 'a').read_text() == 'original\n'
        assert not (vm_manager / 'runtime' / vm_record['container_id']).exists()

    # Missing SDK resources fail clearly, without looking beside the caller or
    # falling back to a helper left in the SDK build tree.
    original_helper = helper.read_bytes()
    helper.unlink()
    error = run([str(consumer), str(manager), str(repo)], expected=1).stderr
    assert 'SDK agentd missing or not executable:' in error and str(helper) in error
    assert 'WRONG_HELPER' not in error
    assert not any(path.name.startswith('bbm-sandbox-') for path in (manager / 'runtime').iterdir())
    assert (repo / 'a').read_text() == 'original\n'
    helper.write_bytes(original_helper)
    helper.chmod(0o755)
    original_runner = runner.read_bytes()
    runner.unlink()
    error = run([str(consumer), str(manager), str(repo)], expected=1).stderr
    assert 'SDK runtime runner missing or not executable:' in error and str(runner) in error
    assert not any(path.name.startswith('bbm-sandbox-') for path in (manager / 'runtime').iterdir())
    runner.write_bytes(original_runner)
    runner.chmod(0o755)
    # Explicit resource root relocates a static SDK's helpers. Private DSOs use
    # $ORIGIN and must not depend on the original installation prefix.
    relocated = root / 'relocated resources'
    shutil.move(str(prefix / 'libexec/bbm-sandbox'), relocated)
    environment = os.environ.copy()
    environment['BBM_SANDBOX_RESOURCE_DIR'] = str(relocated)
    assert run([str(consumer), str(root / 'relocated-manager'), str(repo)], env=environment).stdout == 'installed SDK passed\n'
    if krun_enabled:
        assert run([str(consumer), str(root / 'relocated-vm'), str(repo), 'vm'], env=environment).stdout == 'installed SDK passed\n'
