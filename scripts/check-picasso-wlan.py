#!/usr/bin/env python3
"""Check WLAN wire readers and complete ARM64 probe body; no radio/SDK qualification."""
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
    parser.add_argument('--cc', default=shutil.which('clang'))
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    driver = ROOT / 'drivers/NativeWlan'
    runs = []

    def run(name, command):
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        runs.append(dict(name=name, command=command, passed=True, output=result.stdout))

    for sanitized in (False, True):
        suffix = '-sanitized' if sanitized else ''
        flags = ['-O1', '-Wall', '-Wextra', '-Werror', '-I', str(driver)]
        if os.name == 'nt':
            flags += ['-D_CRT_SECURE_NO_WARNINGS']
        sanitize = (['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined',
                     '-fno-omit-frame-pointer'] if sanitized else [])
        objects = []
        for source in ('Test/main.c', 'WlanFirmware.c', 'WlanBoard.cpp'):
            name = Path(source).stem + suffix
            obj = out / (name + '.o')
            cpp = source.endswith('.cpp')
            run(name, [args.cxx if cpp else args.cc, '-std=c++17' if cpp else '-std=c11',
                       *flags, *sanitize, '-c', str(driver / source), '-o', str(obj)])
            objects.append(str(obj))
        exe = out / ('wlan' + suffix + ('.exe' if os.name == 'nt' else ''))
        run('link' + suffix, [args.cxx, *sanitize, *objects, '-o', str(exe)])
        run('run' + suffix, [str(exe)])

    body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (driver / 'WlanProbe.cpp').read_text(), flags=re.M)
    source = out / 'WlanProbe.cpp'
    source.write_text('#include "picasso-wlan-kernel-api.h"\n' + body)
    run('arm64', [args.cxx, '-target', 'arm64-apple-ios17.0', '-std=c++17',
        '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
        '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'scripts/tests'), '-I', str(driver),
        '-c', str(source), '-o', str(out / 'WlanProbe.o')])
    inputs = [driver / n for n in ('WlanProbe.cpp', 'WlanSmem.h', 'WlanBoard.h', 'WlanBoard.cpp',
                                   'WlanFirmware.h', 'WlanFirmware.c', 'Test/main.c', 'Test/Makefile')]
    inputs += [Path(__file__), ROOT / 'scripts/tests/picasso-wlan-kernel-api.h']
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        hardware_tested=False, kernel_sdk_compiled=False,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}), indent=2) + '\n')
    print('PASS: WLAN bounded wire readers, sanitizer and production ARM64 probe body')


if __name__ == '__main__':
    main()
