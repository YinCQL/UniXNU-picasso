#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute QMP protocol/platform errors and compile the complete ARM64 API model."""
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
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cxx', default=shutil.which('clang++'))
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []

    def run(name, command):
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (out / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf8', newline='\n')
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        runs.append(dict(name=name, passed=True, output=r.stdout))

    body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (ROOT/'package/PicassoAop.cpp').read_text(), flags=re.M)
    (out/'PicassoAop-body.inc').write_text(body, encoding='utf8', newline='\n')
    includes = ['-I', str(out), '-I', str(ROOT/'package'), '-I', str(ROOT/'scripts/tests'), '-I', str(ROOT/'drivers/NativeWlan')]
    core = ROOT/'package/PicassoQmp.cpp'
    for sanitized in (False, True):
        suffix = '-asan' if sanitized else ''
        flags = (['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined',
                  '-fno-omit-frame-pointer'] if sanitized else [])
        for name in ('qmp', 'aop'):
            exe = out/(name + suffix + ('.exe' if os.name == 'nt' else ''))
            run(name + '-compile' + suffix, [a.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                *flags, *includes, str(ROOT/f'scripts/tests/picasso-{name}-host.cpp'), str(core), '-o', str(exe)])
            cases = ('normal',) if name == 'qmp' else (
                'normal', 'alloc-ram', 'alloc-ipcc', 'map-ram', 'map-ipcc', 'timeout', 'close-timeout', 'bad', 'busy', 'rx',
                'load-normal','load-timeout','load-off-timeout','load-close-timeout','load-busy','load-rx',
                'load-bad','load-map','load-handshake-timeout','load-disconnect')
            for case in cases:
                run(name + '-' + case + suffix, [str(exe), case])
    arm = out/'aop-arm.cpp'
    arm.write_text('#include "picasso-wlan-kernel-api.h"\n'
        'extern "C" { void IOLog(const char *,...) __attribute__((format(printf,1,2)));\n'
        'void IOSleep(unsigned); uint64_t mach_absolute_time();\n'
        'void clock_interval_to_deadline(unsigned,unsigned,uint64_t *); }\n'
        'enum {kSecondScale=1};\n' + body, encoding='utf8', newline='\n')
    for name, source in [('qmp', core), ('aop', arm)]:
        run(name + '-arm64', [a.cxx, '-target', 'arm64-apple-ios17.0', '-std=c++17', '-ffreestanding',
            '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-mgeneral-regs-only', '-Wall', '-Wextra',
            '-Werror', *includes, '-c', str(source), '-o', str(out/(name+'.o'))])
    inputs = ['package/PicassoQmp.h', 'package/PicassoQmp.cpp', 'package/PicassoAop.cpp', 'package/PicassoAdspBoot.h',
              'package/PicassoAdspFirmware.cpp', 'scripts/build-picasso-display.py',
              'scripts/check-picasso-qmp.py', 'scripts/tests/picasso-qmp-host.cpp',
              'scripts/tests/picasso-aop-host.cpp', 'scripts/tests/picasso-wlan-kernel-api.h',
              '.github/workflows/host-checks.yml']
    (out/'results.json').write_text(json.dumps(dict(passed=True, hardware_tested=False,
        kernel_sdk_compiled=False, dsp_started=False, runs=runs,
        inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}), indent=2)+'\n',
        encoding='utf8', newline='\n')
    print('PASS QMP wire protocol, AOP lifecycle failures and ARM64 API model')


if __name__ == '__main__':
    main()
