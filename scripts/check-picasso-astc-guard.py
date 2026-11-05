#!/usr/bin/env python3
"""Run production ASTC preflight arithmetic on the host; no device/Apple layout claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixtures = [ROOT / 'scripts/tests' / name for name in
                ('picasso-astc-guard-host.c', 'picasso-astc-status-host.c', 'picasso-inject-probe-host.c')]
    patch = ROOT / 'patches/picasso-astc-status-xnu.patch'
    added = '\n'.join(row[1:] for row in patch.read_text().splitlines()
                      if row.startswith('+') and not row.startswith('+++'))
    handler = re.search(r'static int\npicasso_astc_status_sysctl.*?\n}', added, re.S)[0]
    (out / 'astc-status.inc').write_text(handler + '\n', encoding='utf-8')
    probe_patch = ROOT / 'patches/picasso-inject-open-probe-xnu.patch'
    probe = '\n'.join(row[1:] for row in probe_patch.read_text().splitlines()
                      if row.startswith('+') and not row.startswith('+++'))
    probe = re.search(r'static void\npicasso_inject_open_probe.*?\n}', probe, re.S)[0]
    (out / 'inject-probe.inc').write_text(probe + '\n', encoding='utf-8')
    include = ROOT / 'package/picasso-springboard'
    compiler = os.environ.get('CC') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('clang is required')
    runs = []
    for fixture, sanitized in ((f, s) for f in fixtures for s in (False, True)):
        label = fixture.stem + ('-sanitized' if sanitized else '-normal')
        exe = out / (label + ('.exe' if os.name == 'nt' else ''))
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                   '-I', str(include), '-I', str(out), str(fixture), '-o', str(exe)]
        if sanitized:
            command += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer']
        built = subprocess.run(command, capture_output=True, text=True)
        (out / (label + '-compile.log')).write_text(built.stdout + built.stderr, encoding='utf-8')
        if built.returncode:
            raise RuntimeError(built.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / (label + '-run.log')).write_text(ran.stdout + ran.stderr, encoding='utf-8')
        if ran.returncode:
            raise RuntimeError(ran.stdout + ran.stderr)
        runs.append(dict(fixture=fixture.name, sanitized=sanitized, passed=True, output=ran.stdout))
    inputs = [*fixtures, patch, probe_patch, include / 'picasso-astc-guard-core.h', include / 'picasso-astc-guard.m',
              include / 'picasso-springboard-probe.m', include / 'picasso-statusbar-metrics.m', Path(__file__)]
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}),
        indent=2) + '\n', encoding='utf-8')
    print('PASS: ASTC guard host checks, normal + sanitizers')


if __name__ == '__main__':
    main()
