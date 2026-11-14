#!/usr/bin/env python3
"""Check executed ADSP survey, malformed firmware and complete ARM64 probe body."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []

    def run(name, command, **kwargs):
        r = subprocess.run(command, capture_output=True, text=True, **kwargs)
        (out / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf-8', newline='\n')
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        runs.append(dict(name=name, command=command, passed=True, output=r.stdout))

    body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (ROOT / 'package/PicassoAdsp.cpp').read_text(), flags=re.M)
    (out / 'PicassoAdsp-body.inc').write_text(body, encoding='utf-8', newline='\n')
    include = ['-I', str(out), '-I', str(ROOT / 'package'), '-I', str(ROOT / 'drivers/NativeWlan')]
    for sanitized in (False, True):
        suffix = '-sanitized' if sanitized else ''
        flags = (['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else [])
        exe = out / ('adsp' + suffix + ('.exe' if os.name == 'nt' else ''))
        run('compile' + suffix, [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
            *include, str(ROOT / 'scripts/tests/picasso-adsp-host.cpp'), '-o', str(exe)])
        for case in ('normal', 'alloc', 'map'):
            run(case + suffix, [str(exe), case])
    source = out / 'PicassoAdsp.cpp'
    source.write_text('#include "picasso-wlan-kernel-api.h"\nextern "C" void IOLog(const char *, ...) __attribute__((format(printf, 1, 2)));\n' + body,
                      encoding='utf-8', newline='\n')
    run('arm64', [args.cxx, '-target', 'arm64-apple-ios17.0', '-std=c++17', '-ffreestanding',
        '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-mgeneral-regs-only',
        '-Wall', '-Wextra', '-Werror', *include, '-I', str(ROOT / 'scripts/tests'),
        '-c', str(source), '-o', str(out / 'PicassoAdsp.o')])
    env = dict(os.environ, PYTHONPATH=str(ROOT / 'scripts'), PYTHONDONTWRITEBYTECODE='1')
    run('firmware', [sys.executable, '-B', str(ROOT / 'scripts/tests/picasso-adsp-firmware.py')], env=env)
    pr13 = (ROOT / 'package/PicassoPr13.cpp').read_text()
    assert pr13.count('picasso_adsp_poll();') == 1
    assert "ROOT / 'kits/picasso/PicassoAdsp.cpp'" in (ROOT / 'scripts/build-picasso-display.py').read_text()
    inputs = ['package/PicassoAdsp.cpp', 'package/PicassoAdsp.h', 'package/PicassoPr13.cpp',
              'drivers/NativeWlan/WlanSmem.h', 'scripts/build-picasso-display.py',
              'scripts/picasso_adsp_firmware.py', 'scripts/prepare-picasso-adsp.py',
              'scripts/check-picasso-adsp.py', 'scripts/tests/picasso-adsp-host.cpp',
              'scripts/tests/picasso-adsp-firmware.py', 'scripts/tests/picasso-wlan-kernel-api.h']
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        hardware_tested=False, kernel_sdk_compiled=False, dsp_started=False,
        inputs={p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in inputs}), indent=2) + '\n', encoding='utf-8', newline='\n')
    print('PASS ADSP production survey, failures, firmware bounds and ARM64 API model')


if __name__ == '__main__':
    main()
