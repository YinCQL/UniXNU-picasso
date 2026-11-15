#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production AMIC1 hardware core against bus and restore faults."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    names = ['package/PicassoMicHardware.cpp', 'package/PicassoMicHardware.h',
             'scripts/tests/picasso-mic-hardware-host.cpp',
             'scripts/check-picasso-mic-hardware.py']
    result = dict(passed=False, hardware_tested=False, runs=[],
                  inputs={n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest() for n in names})
    try:
        for sanitized in (False, True):
            suffix = '-asan' if sanitized else ''
            binary = out / ('mic-hardware' + suffix)
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-fno-omit-frame-pointer'] if sanitized else []
            commands = [('compile' + suffix, [args.cxx, '-std=c++14', '-O1',
                         '-DPICASSO_MIC_HARDWARE_HOST', '-Wall', '-Wextra', '-Werror',
                         *flags, str(ROOT / names[0]), str(ROOT / names[2]), '-o', str(binary)]),
                        ('hardware' + suffix, [str(binary)])]
            for name, command in commands:
                run = subprocess.run(command, capture_output=True, text=True, timeout=120)
                (out / (name + '.log')).write_text(run.stdout + run.stderr)
                result['runs'].append(dict(name=name, command=command, exit_code=run.returncode,
                                           passed=run.returncode == 0))
                if run.returncode:
                    raise RuntimeError(name + ': ' + run.stdout + run.stderr)
        result['passed'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS AMIC1 hardware core, SoundWire faults and ordered restoration; host model only')


if __name__ == '__main__':
    main()
