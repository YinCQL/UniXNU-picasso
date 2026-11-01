#!/usr/bin/env python3
"""Check the real gesture planner with host compilation; not device qualification."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang++') or shutil.which('clang'))
    args = parser.parse_args()
    if not args.cc:
        parser.error('An installed Clang is required')
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []
    for asan in (False, True):
        name = 'hid-plan-asan' if asan else 'hid-plan'
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        command = [args.cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0', '-g',
                   '-I', str(root / 'package'), str(root / 'scripts/tests/picasso-hid-plan-host.cpp'), '-o', str(exe)]
        if asan:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (name + '-compile.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / (name + '-run.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        runs.append(dict(asan=asan, command=command, output=result.stdout.strip(), passed=True))
    (out / 'results.json').write_text(json.dumps(dict(scope=__doc__, runs=runs, passed=True), indent=2) + '\n', encoding='utf-8')
    print('PASS: HID planner normal and ASan checks')


if __name__ == '__main__':
    main()
