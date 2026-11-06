#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute device firmware/FAT/UFS bodies; optional read-only stock ROM validation."""
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
    p.add_argument('--stock', type=Path)
    p.add_argument('--cxx', default=shutil.which('clang++'))
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []

    def run(name, command):
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (out / (name+'.log')).write_text(r.stdout+r.stderr, encoding='utf8', newline='\n')
        if r.returncode:
            raise RuntimeError(r.stdout+r.stderr)
        runs.append(dict(name=name, passed=True, output=r.stdout))

    body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (ROOT/'package/PicassoAdspFirmware.cpp').read_text(), flags=re.M)
    (out/'PicassoAdspFirmware-body.inc').write_text(body, encoding='utf8', newline='\n')
    includes = ['-I', str(out), '-I', str(ROOT/'package'), '-I', str(ROOT/'drivers/NativeUfs')]
    for sanitizer in (False, True):
        suffix = '-asan' if sanitizer else ''
        flags = ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitizer else []
        for name, sources in [('firmware', [ROOT/'scripts/tests/picasso-firmware-host.cpp', ROOT/'package/PicassoFat.cpp']),
                              ('modem', [ROOT/'scripts/tests/picasso-firmware-ufs.cpp']),
                              ('gpt', [ROOT/'scripts/tests/picasso-firmware-gpt.cpp'])]:
            exe = out/(name+suffix+('.exe' if os.name == 'nt' else ''))
            libs = (['-lbcrypt'] if os.name == 'nt' else ['-lcrypto']) if name == 'firmware' else []
            run(name+'-compile'+suffix, [a.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                *includes, *map(str,sources), *libs, '-o', str(exe)])
            if name != 'firmware':
                run(name+suffix, [str(exe)])
            else:
                for case in ('partition','alloc','thread','bad-volume','not-ready','late-ready'):
                    run(case+suffix, [str(exe), case])
                if a.stock:
                    run('stock'+suffix, [str(exe), 'stock', str(a.stock)])
    arm = out/'firmware-arm.cpp'
    arm.write_text('#include "picasso-firmware-kernel-api.h"\n'+body, encoding='utf8', newline='\n')
    for name, source in [('firmware',arm), ('fat',ROOT/'package/PicassoFat.cpp')]:
        run(name+'-arm64', [a.cxx, '-target', 'arm64-apple-ios17.0', '-std=c++17', '-ffreestanding', '-fno-exceptions',
            '-fno-rtti', '-fno-threadsafe-statics', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', *includes,
            '-I', str(ROOT/'scripts/tests'), '-c', str(source), '-o', str(out/(name+'.o'))])
    inputs = ['package/PicassoFat.cpp', 'package/PicassoFat.h', 'package/PicassoAdspFirmware.cpp',
              'package/PicassoAdspFirmwareProfile.h', 'drivers/NativeUfs/FirmwareRead.h',
              'drivers/NativeUfs/NativeUfs.cpp', 'drivers/NativeUfs/RootLayout.h', 'package/PicassoPr13.cpp',
              'scripts/build-picasso-display.py', 'scripts/check-picasso-firmware.py',
              'scripts/tests/picasso-firmware-host.cpp', 'scripts/tests/picasso-firmware-ufs.cpp',
              'scripts/tests/picasso-firmware-kernel-api.h', 'scripts/tests/picasso-firmware-gpt.cpp']
    (out/'results.json').write_text(json.dumps(dict(passed=True, hardware_tested=False, kernel_sdk_compiled=False,
        dsp_started=False, runs=runs, inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}),indent=2)+'\n', encoding='utf8', newline='\n')
    print('PASS firmware worker, FAT failures, modem bounds and ARM64 API model')


if __name__ == '__main__':
    main()
