#!/usr/bin/env python3
"""Reuse Ubuntu runner tools and APT archives before checking the pinned host ABI.

This consumer helper does not alter setup-toolchain.py or its Release recipe.
APT still resolves dependencies and verifies signed indexes/package hashes.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

MIRRORS = ('http://azure.archive.ubuntu.com/ubuntu', 'https://archive.ubuntu.com/ubuntu')
# Command-provided dependencies can already exist outside dpkg (notably cmake).
# Library/header packages still require dpkg; the final upstream compile/link
# probe checks the compiler/libclang pair, headers and linked host libraries.
COMMANDS = {
    'git': ('git',), 'patch': ('patch',), 'flex': ('flex',), 'bison': ('bison',),
    'bzip2': ('bzip2',), 'cpio': ('cpio',), 'rsync': ('rsync',), 'curl': ('curl',),
    'wget': ('wget',), 'tcsh': ('tcsh',), 'unifdef': ('unifdef',), 'cmake': ('cmake',),
    'lld': ('ld.lld',), 'mtools': ('mformat', 'mcopy'), 'pkg-config': ('pkg-config',),
    'ninja-build': ('ninja',), 'python3': ('python3',), 'xz-utils': ('xz',),
    'device-tree-compiler': ('dtc', 'fdtget', 'fdtput'),
    'build-essential': ('cc', 'c++', 'make', 'ld', 'ar'),
}
ALIASES = {'clang': 'clang-18', 'llvm-dev': 'llvm-18-dev', 'libclang-dev': 'libclang-18-dev'}


def installed(package):
    r = subprocess.run(['dpkg-query', '-W', '-f=${db:Status-Abbrev}', package],
                       capture_output=True, text=True, timeout=15)
    return r.returncode == 0 and r.stdout.strip() == 'ii'


def missing_packages(packages, has_package=installed, which=shutil.which):
    missing = []
    for name in dict.fromkeys(packages):
        if name in COMMANDS:
            if all(which(command) for command in COMMANDS[name]):
                continue
        elif has_package(name):
            continue
        if name in ALIASES and has_package(ALIASES[name]):
            # llvm-config without a suffix is used by the fixed Release identity.
            if name != 'llvm-dev' or which('llvm-config'):
                continue
        missing.append(name)
    return missing


def run(command, **kw):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kw)


def probe_path():
    # Probe a real large package from the runner's existing APT metadata, not a
    # tiny index response. The bytes are discarded; APT does the trusted fetch.
    r = subprocess.run(['apt-cache', 'show', 'libclang-18-dev'], capture_output=True,
                       text=True, timeout=15)
    for line in r.stdout.splitlines():
        if line.startswith('Filename: '):
            path = line.removeprefix('Filename: ')
            if path.startswith('pool/') and all(c.isalnum() or c in '/._+~-' for c in path) and '..' not in path:
                return path
    return None


def probe(origin, path):
    try:
        r = subprocess.run(['curl', '--silent', '--show-error', '--fail', '--connect-timeout', '3',
                            '--max-time', '8', '--range', '0-1048575', '--max-filesize', '1048576',
                            '--output', os.devnull, '--write-out', '%{http_code} %{size_download} %{speed_download}',
                            origin + '/' + path], capture_output=True, text=True, timeout=12)
    except (OSError, subprocess.TimeoutExpired):
        return {'origin': origin, 'bytes_per_second': 0, 'curl_exit': -1}
    try:
        code, size, speed = r.stdout.split()
        # A slow but useful partial transfer (curl timeout 28) can be compared.
        valid = r.returncode in (0, 28) and code in ('200', '206') and 262144 <= int(size) <= 1048576
        return {'origin': origin, 'bytes_per_second': float(speed) if valid else 0,
                'bytes': int(size), 'curl_exit': r.returncode}
    except ValueError:
        return {'origin': origin, 'bytes_per_second': 0, 'curl_exit': r.returncode}


def choose_mirror(results):
    useful = [r for r in results if r['bytes_per_second'] > 0]
    return max(useful, key=lambda r: r['bytes_per_second'])['origin'] if useful else MIRRORS[1]


def apt_sources(origin):
    if origin not in MIRRORS:
        raise ValueError('Only the two Ubuntu official mirrors are supported')
    options = '[arch=amd64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg]'
    rows = [f'deb {options} {origin} {suite} main restricted universe multiverse'
            for suite in ('noble', 'noble-updates', 'noble-backports')]
    rows.append(f'deb {options} https://security.ubuntu.com/ubuntu noble-security main restricted universe multiverse')
    return '\n'.join(rows) + '\n'


def apt_options(source, lists, cache):
    return ['-o', f'Dir::Etc::sourcelist={source}', '-o', 'Dir::Etc::sourceparts=-',
            '-o', f'Dir::State::lists={lists}', '-o', f'Dir::Cache::archives={cache}',
            '-o', 'Acquire::Retries=2', '-o', 'Acquire::http::Timeout=20',
            '-o', 'Acquire::https::Timeout=20', '-o', 'Acquire::Languages=none',
            '-o', 'APT::Keep-Downloaded-Packages=true', '-o', 'Binary::apt-get::APT::Keep-Downloaded-Packages=true']


def install(tree, cache, report):
    if platform.system() != 'Linux' or platform.machine() != 'x86_64' or os.geteuid():
        raise ValueError('Run as root on the Ubuntu 24.04 x86_64 runner')
    release = platform.freedesktop_os_release()
    if release.get('ID') != 'ubuntu' or release.get('VERSION_ID') != '24.04':
        raise ValueError('The fixed toolchain Release requires Ubuntu 24.04')
    script = tree / 'scripts/linux/setup-toolchain.py'
    check = [sys.executable, str(script), '--check']
    result = {'passed': False, 'apt_used': False, 'missing_packages': [], 'mirrors': []}
    begin = time.monotonic()
    try:
        print('Checking preinstalled host compiler, tools and libraries first', flush=True)
        if subprocess.run(check, timeout=180).returncode == 0:
            result['passed'] = True
            return
        spec = importlib.util.spec_from_file_location('linux_setup', script)
        setup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(setup)
        packages = setup.COMMON_PACKAGES + setup.PACKAGES['apt']
        result['missing_packages'] = missing_packages(packages)
        print('Missing host packages: ' + ', '.join(result['missing_packages']), flush=True)
        if not result['missing_packages']:
            raise RuntimeError('Host check failed with all dependencies present; refusing a blind upgrade')
        path = probe_path()
        if path and shutil.which('curl'):
            with ThreadPoolExecutor(max_workers=2) as pool:
                result['mirrors'] = list(pool.map(lambda origin: probe(origin, path), MIRRORS))
        origin = choose_mirror(result['mirrors'])
        result['selected_mirror'] = origin
        print('Ubuntu mirror measurements: ' + json.dumps(result['mirrors']), flush=True)
        print('Selected official Ubuntu mirror: ' + origin, flush=True)
        cache.mkdir(parents=True, exist_ok=True)
        result['cached_archives'] = len(list(cache.glob('*.deb')))
        result['apt_used'] = True
        # Source/index overrides apply only to these invocations, not system
        # configuration. Third-party runner feeds are unnecessary for these deps.
        with tempfile.TemporaryDirectory(prefix='picasso-apt-') as tmp:
            directory = Path(tmp)
            directory.chmod(0o755)
            source = directory / 'ubuntu.list'
            source.write_text(apt_sources(origin), encoding='utf8')
            lists = directory / 'lists'
            lists.mkdir(mode=0o755)
            options = apt_options(source, lists, cache)
            env = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
            run(['apt-get', *options, '-o', 'APT::Update::Error-Mode=any', 'update'], env=env)
            run(['apt-get', *options, 'install', '-y', '--no-install-recommends', '--no-upgrade',
                 *result['missing_packages']], env=env)
        run(check, timeout=180)
        result['retained_archives'] = len(list(cache.glob('*.deb')))
        result['passed'] = True
    finally:
        result['elapsed_seconds'] = round(time.monotonic() - begin, 2)
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--cache-dir', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    install(a.tree.resolve(), a.cache_dir.resolve(), a.report.resolve())
