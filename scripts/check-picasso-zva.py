#!/usr/bin/env python3
"""Exercise the production DC ZVA qualification and BSD process bridge with host models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    path = root / 'patches/unixnu-source-kernel.patch'
    section = path.read_text(encoding='utf-8').split(
        'diff --git a/patches/xnu/zzzzzz-picasso-zva-fallback.patch ', 1)[1]
    added = '\n'.join(line[2:] for line in section.splitlines() if line.startswith('++'))
    helper = re.search(r'static bool\npicasso_user_zva\(.*?\n}', added, re.S)[0]
    (out / 'zva.inc').write_text(helper, encoding='utf-8')
    process = re.search(r'int\npicasso_ui_process_pid\(void\)\n\{.*?\n}', added, re.S)[0]
    (out / 'process.inc').write_text(process, encoding='utf-8')
    fixture = root / 'scripts/tests/picasso-zva-host.c'
    shutil.copyfile(fixture, out / 'fixture.c')
    compiler = os.environ.get('CC') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('clang is required')
    runs = []
    for asan in (False, True):
        label = 'asan' if asan else 'normal'
        exe = out / (label + ('.exe' if os.name == 'nt' else ''))
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                   str(out / 'fixture.c'), '-o', str(exe)]
        if asan:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (label + '-compile.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / (label + '-run.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        runs.append(dict(asan=asan, pass_=True, log=result.stdout))
    (out / 'results.json').write_text(json.dumps(dict(pass_=True, scope=__doc__, runs=runs,
        inputs={str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (path, fixture, Path(__file__))}), indent=2) + '\n', encoding='utf-8')
    print('PASS: DC ZVA qualification, rejected faults preserve state; normal + ASan')


if __name__ == '__main__':
    main()
