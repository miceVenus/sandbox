#!/usr/bin/env python3
"""Build matching runtime development files without sudo or modifying system installs."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import urllib.request

PROJECT = Path(__file__).resolve().parents[1]


def run(args, cwd=None, env=None):
    print('+ ' + shlex.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def output(args, cwd=None):
    return subprocess.check_output(list(map(str, args)), cwd=cwd, text=True).strip()


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def checkout(name, spec, cache, offline):
    path = cache / 'source' / (name + '-' + spec['commit'][:12])
    if not (path / '.git').exists():
        if offline:
            raise RuntimeError(f'Offline source cache missing: {path}')
        path.mkdir(parents=True, exist_ok=True)
        run(['git', 'init', '-q', path])
        run(['git', '-C', path, 'remote', 'add', 'origin', spec['repository']])
    try:
        current = output(['git', '-C', path, 'rev-parse', 'HEAD'])
    except subprocess.CalledProcessError:
        if offline:
            raise RuntimeError(f'Offline source checkout incomplete: {path}')
        run(['git', '-C', path, 'fetch', '--depth=1', 'origin', spec['commit']])
        run(['git', '-C', path, 'checkout', '--detach', 'FETCH_HEAD'])
        current = output(['git', '-C', path, 'rev-parse', 'HEAD'])
    if current != spec['commit'] or output(['git', '-C', path, 'diff', '--name-only', 'HEAD']):
        raise RuntimeError(f'Dependency checkout was modified: {path}; keep local changes and use another cache')
    if (path / '.gitmodules').exists():
        if not offline:
            run(['git', '-C', path, 'submodule', 'update', '--init', '--recursive'])
        status = output(['git', '-C', path, 'submodule', 'status', '--recursive'])
        if any(line[:1] in ('-', '+', 'U') for line in status.splitlines()):
            raise RuntimeError(f'Submodules do not match their pinned commits: {path}')
    if name == 'crun' and output(['git', '-C', path / 'libocispec', 'rev-parse', 'HEAD']) != spec['libocispec_commit']:
        raise RuntimeError('crun/libocispec versions do not match deps.lock.json')
    return path


def kernel_archive(spec, cache, offline):
    entry = spec['kernel']
    path = cache / 'downloads' / entry['url'].rsplit('/', 1)[-1]
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        if offline:
            raise RuntimeError(f'Offline kernel archive missing: {path}')
        temporary = path.with_suffix(path.suffix + '.part')
        try:
            with urllib.request.urlopen(entry['url'], timeout=60) as source, temporary.open('wb') as target:
                shutil.copyfileobj(source, target)
            if digest(temporary) != entry['sha256']:
                raise RuntimeError('Downloaded kernel archive checksum mismatch')
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(path) != entry['sha256']:
        raise RuntimeError(f'Kernel cache checksum mismatch: {path}')
    return path


def record_path(prefix, name):
    return prefix / 'share' / 'bbm-sandbox-deps' / (name + '.json')


def cached(prefix, name, identity):
    path = record_path(prefix, name)
    try:
        record = json.loads(path.read_text())
        return record['identity'] == identity and all(
            (prefix / file).is_file() and digest(prefix / file) == checksum
            for file, checksum in record['files'].items())
    except (OSError, KeyError, ValueError):
        return False


def save_record(prefix, name, identity, files):
    record = {'schema': 1, 'identity': identity,
              'files': {str(file.relative_to(prefix)): digest(file) for file in files}}
    path = record_path(prefix, name)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(record, indent=2) + '\n')
    temporary.replace(path)


def copy_headers(source, target):
    files = []
    for header in source.rglob('*.h'):
        destination = target / header.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(header, destination)
        files.append(destination)
    return files


def build_crun(source, spec, cache, prefix, jobs, env, offline):
    # autogen.sh performs another network-capable submodule update; we have already
    # checked out the pinned tree, so run autoreconf directly, including libocispec.
    run(['autoreconf', '-fi'], source, env)
    build = cache / 'build' / ('crun-' + spec['commit'][:12])
    build.mkdir(parents=True, exist_ok=True)
    run([source / 'configure', f'--prefix={prefix}', f'--libdir={prefix / "lib"}', *spec['configure']], build, env)
    # Explicit library targets bypass Automake's all/BUILT_SOURCES prepass.
    # Finish schema and version generation before parallel compilation starts;
    # the library's link dependency alone does not order header generation.
    run(['make', '-C', 'libocispec', f'-j{jobs}', 'generate'], build, env)
    run(['make', f'-j{jobs}', '.version', 'git-version.h'], build, env)
    run(['make', f'-j{jobs}', 'libcrun.la'], build, env)
    (prefix / 'lib').mkdir(parents=True, exist_ok=True)
    library = prefix / 'lib/libcrun.a'
    shutil.copy2(build / '.libs/libcrun.a', library)
    files = [library]
    files += copy_headers(source / 'src/libcrun', prefix / 'include/libcrun')
    files += copy_headers(source / 'libocispec/src/ocispec', prefix / 'include/ocispec')
    files += copy_headers(build / 'libocispec/src/ocispec', prefix / 'include/ocispec')
    config = prefix / 'include/bbm-sandbox-deps/crun/config.h'
    config.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / 'config.h', config)
    files.append(config)
    return files


def build_krun(name, source, spec, cache, prefix, jobs, env, offline):
    if name == 'libkrunfw':
        archive = kernel_archive(spec, cache, offline)
        target = source / 'tarballs' / archive.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            target.symlink_to(archive)
        if digest(target) != spec['kernel']['sha256']:
            raise RuntimeError('libkrunfw source kernel archive differs from the lock file')
    variables = [f'PREFIX={prefix}', 'LIBDIR_Linux=lib', *spec.get('make', [])]
    run(['make', f'-j{jobs}', *variables], source, env)
    run(['make', *variables, 'install'], source, env)
    library = prefix / 'lib' / spec['soname']
    # Only modify this script's installed copy, never an external dependency.
    run(['patchelf', '--set-rpath', '$ORIGIN', library], env=env)
    files = list((prefix / 'lib').glob(name + '.so*'))
    if name == 'libkrun':
        files += list((prefix / 'include').glob('libkrun*.h'))
        files.append(prefix / 'lib/pkgconfig/libkrun.pc')
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=PROJECT / '.deps')
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument('--without-krun', action='store_true', help='Build only libcrun development files')
    parser.add_argument('--offline', action='store_true', help='Require cached sources, kernel and Cargo packages')
    parser.add_argument('--rebuild', action='store_true', help='Ignore completed artifact manifests')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'aarch64'):
        raise RuntimeError('Dependency bootstrap currently supports native Linux x86_64/aarch64')
    cache = args.directory.resolve()
    prefix = args.prefix.resolve() if args.prefix else cache / 'prefix'
    if args.jobs < 1 or any(c.isspace() for c in str(cache) + str(prefix)):
        raise RuntimeError('Use a positive job count and dependency paths without whitespace (upstream Makefiles)')
    tools = ['git', 'make', 'cc', 'pkg-config', 'autoreconf', 'automake', 'libtoolize', 'python3']
    if not args.without_krun:
        tools += ['cargo', 'rustc', 'clang', 'patchelf', 'bison', 'flex', 'patch', 'tar']
        llvm_config = os.environ.get('LLVM_CONFIG_PATH', 'llvm-config')
        if not shutil.which(llvm_config):
            raise RuntimeError(f'Missing LLVM configuration tool: {llvm_config}; see README.md')
    missing = [tool for tool in tools if not shutil.which(tool)]
    if missing:
        raise RuntimeError('Missing build tools: ' + ', '.join(missing) + '; see README.md')
    run(['pkg-config', '--exists', 'libsystemd', 'libseccomp', 'libcap', 'json-c'])
    lock = json.loads((PROJECT / 'deps.lock.json').read_text())
    if lock['schema'] != 1:
        raise RuntimeError('Unsupported dependency lock schema')
    cache.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(parents=True, exist_ok=True)
    with (prefix / '.build.lock').open('w') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        env = os.environ.copy()
        env['CARGO_BUILD_JOBS'] = str(args.jobs)
        if args.offline:
            env['CARGO_NET_OFFLINE'] = 'true'
        # Keep native linker inputs separate from Cargo flags; no global loader installation.
        env['LIBRARY_PATH'] = str(prefix / 'lib') + ':' + env.get('LIBRARY_PATH', '')
        env['LD_LIBRARY_PATH'] = str(prefix / 'lib') + ':' + env.get('LD_LIBRARY_PATH', '')
        compiler = output(shlex.split(env.get('CC', 'cc')) + ['--version']).splitlines()[0]
        toolchain = {'system': platform.system(), 'machine': platform.machine(), 'compiler': compiler,
                     'script': digest(Path(__file__)), 'prefix': str(prefix),
                     'flags': {key: env.get(key, '') for key in ('CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'LIBCLANG_PATH', 'LLVM_CONFIG_PATH')}}
        if not args.without_krun:
            toolchain['rust'] = output(['rustc', '--version'])
        names = ['crun'] if args.without_krun else ['crun', 'libkrunfw', 'libkrun']
        for name in names:
            spec = lock[name]
            identity = {'source': spec, 'toolchain': toolchain}
            if name == 'libkrun':
                identity['firmware'] = lock['libkrunfw']
            if not args.rebuild and cached(prefix, name, identity):
                print(f'{name}: matching development files already installed', flush=True)
                continue
            record_path(prefix, name).unlink(missing_ok=True)
            source = checkout(name, spec, cache, args.offline)
            if name == 'crun':
                files = build_crun(source, spec, cache, prefix, args.jobs, env, args.offline)
            else:
                files = build_krun(name, source, spec, cache, prefix, args.jobs, env, args.offline)
            save_record(prefix, name, identity, files)
        print(f'Dependencies ready: {prefix}\nConfigure with -DSANDBOX_DEPS_PREFIX={prefix}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f'Dependency build failed: {error}', file=sys.stderr)
        sys.exit(1)
