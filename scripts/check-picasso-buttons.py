#!/usr/bin/env python3
"""Check production PMIC/button logic and ARM64 bodies; no SDK/device qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract(text, marker):
    start = text.index(marker)
    cursor, depth = text.index('{', start) + 1, 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang++') or shutil.which('clang'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    hid = (ROOT / 'package/PicassoHidInput.cpp').read_text()
    consumer = (ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp').read_text()
    sink = hid[hid.index('static IOLock *buttonLock;'):hid.index('extern "C" void picasso_hid_physical_button(')]
    (out / 'button-sink.inc').write_text(sink)
    (out / 'button-consumer.inc').write_text('\n'.join(extract(consumer, n) for n in
        ('static bool buttonsNotify(', 'static IOReturn buttonsAction(', 'static IOReturn stopButtons(')))
    runs = []

    def run(name, cmd, execute=False):
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out / (name + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        runs.append(dict(name=name, command=cmd, passed=True, output=r.stdout))
        if execute:
            run(name + '-run', [cmd[cmd.index('-o') + 1]])

    for source in ('picasso-buttons-host.cpp', 'picasso-buttons-dispatch-host.cpp'):
        for sanitized in (False, True):
            name = Path(source).stem + ('-sanitized' if sanitized else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            cmd = [args.cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1',
                   '-I', str(ROOT / 'package'), '-I', str(out),
                   str(ROOT / 'scripts/tests' / source), '-o', str(exe)]
            if sanitized:
                cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            run(name, cmd, True)

    # Compile entire production worker/bridge bodies, including inline ARM64 MMIO
    # barriers. These explicit API declarations are not a substitute for XNU headers.
    for name in ('PicassoHidInput', 'PicassoButtons'):
        body = (ROOT / 'package' / (name + '.cpp')).read_text()
        body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', body, flags=re.M)
        source = out / (name + '.cpp')
        source.write_text('#include "picasso-input-kernel-api.h"\n' + body)
        run(name + '-arm64', [args.cc, '-target', 'arm64-apple-ios17.0', '-std=c++17',
            '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
            '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-I', str(ROOT / 'scripts/tests'), '-I', str(ROOT / 'package'),
            '-I', str(ROOT / 'drivers/NativeNovatekTouch'), '-c', str(source), '-o', str(out / (name + '.o'))])
    inputs = [ROOT / 'package' / n for n in ('PicassoButtonsCore.h', 'PicassoButtons.cpp', 'PicassoHidInput.cpp')]
    inputs += [ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp', Path(__file__)]
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        hardware_tested=False, kernel_sdk_compiled=False,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}), indent=2) + '\n')
    print('PASS: PMIC protocol, button dispatch lifecycle, sanitizer tests and production ARM64 bodies')


if __name__ == '__main__':
    main()
