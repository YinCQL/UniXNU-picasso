#!/usr/bin/env python3
"""Check dependency reuse, official mirror isolation and optional live Ubuntu install."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--tree', type=Path)
    p.add_argument('--live', action='store_true')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    script = ROOT/'scripts/install-picasso-linux-host.py'
    spec = importlib.util.spec_from_file_location('installer', script)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    runs = []
    packages = ['cmake', 'libclang-dev', 'llvm-dev', 'ca-certificates', 'clang', 'flex']
    available = {'libclang-18-dev', 'llvm-18-dev', 'ca-certificates', 'clang-18'}
    tools = {'cmake', 'llvm-config'}
    assert m.missing_packages(packages, available.__contains__, tools.__contains__) == ['flex']
    assert m.missing_packages(['llvm-dev'], available.__contains__, lambda _: False) == ['llvm-dev']
    assert m.missing_packages(['cmake', 'cmake'], lambda _: False, lambda _: False) == ['cmake']
    runs.append({'name': 'preinstalled commands and equivalent LLVM packages', 'passed': True})
    azure, official = m.MIRRORS
    assert m.choose_mirror([{'origin': azure, 'bytes_per_second': 10}, {'origin': official, 'bytes_per_second': 1000}]) == official
    assert m.choose_mirror([{'origin': azure, 'bytes_per_second': 2000}, {'origin': official, 'bytes_per_second': 1000}]) == azure
    assert m.choose_mirror([]) == official
    for result, speed in [(subprocess.CompletedProcess([], 0, '206 1048576 1000000'), 1000000),
                          (subprocess.CompletedProcess([], 28, '206 300000 40000'), 40000),
                          (subprocess.CompletedProcess([], 22, '404 300000 40000'), 0),
                          (subprocess.CompletedProcess([], 0, '200 30000000 40000'), 0)]:
        with patch.object(m.subprocess, 'run', return_value=result):
            assert m.probe(official, 'pool/test.deb')['bytes_per_second'] == speed
    with patch.object(m.subprocess, 'run', side_effect=subprocess.TimeoutExpired('curl', 12)):
        assert m.probe(official, 'pool/test.deb')['bytes_per_second'] == 0
    for origin in m.MIRRORS:
        sources = m.apt_sources(origin)
        assert sources.count('signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg') == 4
        assert 'noble-security' in sources and 'trusted=yes' not in sources
    try:
        m.apt_sources('https://untrusted.invalid')
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown mirror accepted')
    runs.append({'name': 'bounded package throughput probes and signed official sources', 'passed': True})
    # Exercise the actual orchestration: ready = zero apt/network; missing =
    # isolated update/install then real check; broken existing ABI = hard fail.
    tree = out/'tree'
    (tree/'scripts/linux').mkdir(parents=True)
    (tree/'scripts/linux/setup-toolchain.py').write_text("COMMON_PACKAGES=['cmake','flex']\nPACKAGES={'apt':[]}\n")
    for mode in ('ready', 'missing', 'broken', 'check-fails'):
        calls = []
        def command(args, **kw):
            calls.append(list(map(str, args)))
            code = 0 if mode == 'ready' else 1
            if '--check' in args and len(calls) > 1:
                code = int(mode == 'check-fails')
            if kw.get('check') and code and '--check' in args:
                raise subprocess.CalledProcessError(code, args)
            return SimpleNamespace(returncode=code if '--check' in args else 0)
        report = out/(mode+'.json')
        with patch.object(m.platform, 'system', return_value='Linux'), \
             patch.object(m.platform, 'machine', return_value='x86_64'), \
             patch.object(m.platform, 'freedesktop_os_release', return_value={'ID':'ubuntu','VERSION_ID':'24.04'}, create=True), \
             patch.object(m.os, 'geteuid', return_value=0, create=True), \
             patch.object(m.subprocess, 'run', side_effect=command), \
             patch.object(m, 'missing_packages', return_value=[] if mode=='broken' else ['flex']), \
             patch.object(m, 'probe_path', return_value=None):
            try:
                m.install(tree, out/'cache', report)
            except (RuntimeError, subprocess.CalledProcessError):
                assert mode in ('broken', 'check-fails')
            else:
                assert mode in ('ready', 'missing')
        data = json.loads(report.read_bytes())
        assert data['passed'] == (mode in ('ready', 'missing'))
        apt = [c for c in calls if c[0] == 'apt-get']
        if mode in ('ready', 'broken'):
            assert not apt and len(calls) == 1
        else:
            assert len(apt) == 2 and '--no-upgrade' in apt[1] and apt[1][-1] == 'flex'
            assert 'Dir::Etc::sourceparts=-' in apt[0]
            assert any(x.startswith('Dir::State::lists=') for x in apt[0])
            assert calls[-1][-1] == '--check'
        runs.append({'name': mode, 'passed': True})
    if a.live:
        if not a.tree or sys.platform != 'linux':
            raise ValueError('--live needs Linux and --tree')
        cache = Path(os.environ['RUNNER_TEMP'])/'picasso-host-test-archives'
        for attempt in (1, 2):
            live = out/f'live-{attempt}.json'
            log = out/f'live-{attempt}.log'
            with log.open('w') as stream:
                process = subprocess.run(['sudo', sys.executable, '-B', str(script), '--tree', str(a.tree.resolve()),
                                          '--cache-dir', str(cache), '--report', str(live)], stdout=stream,
                                         stderr=subprocess.STDOUT, timeout=900)
            if process.returncode:
                raise RuntimeError(log.read_text(errors='replace')[-16000:])
            data = json.loads(live.read_bytes())
            assert data['passed'] and (attempt == 1 or not data['apt_used'])
            print('Live host verification: ' + json.dumps(data), flush=True)
            runs.append({'name': f'live-ubuntu-{attempt}', 'passed': True,
                         'apt_used': data['apt_used'], 'seconds': data['elapsed_seconds'], 'details': data})
    names = ['scripts/install-picasso-linux-host.py', 'scripts/check-picasso-linux-host.py',
             '.github/workflows/build-kernel-linux.yml', '.github/workflows/host-checks.yml']
    (out/'results.json').write_text(json.dumps({'passed':True, 'live_ubuntu_tested':a.live, 'runs':runs,
        'inputs':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}},indent=2)+'\n',encoding='utf8')
    print('PASS Linux dependency reuse, mirror selection and install verification; live Ubuntu:', a.live)


if __name__ == '__main__':
    main()
