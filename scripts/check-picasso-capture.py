#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production APR capture state machine with bounded peer responses."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []
    cc = os.environ.get('PICASSO_CAPTURE_HOST_CXX', '/usr/bin/clang++-18')
    paths = ['package/PicassoCapture.h', 'package/PicassoCaptureApr.h',
             'scripts/tests/picasso-capture-host.cpp', 'scripts/check-picasso-capture.py']
    result = dict(passed=False, hardware_tested=False, runs=runs,
                  inputs={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in paths})
    def run(name, command):
        proc = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (out / (name + '.log')).write_text(proc.stdout + proc.stderr)
        runs.append(dict(name=name, command=command, exit_code=proc.returncode, passed=proc.returncode == 0))
        if proc.returncode:
            raise RuntimeError(name + ': ' + proc.stdout + proc.stderr)
        print(name, 'PASS', flush=True)
    try:
        for sanitized in (False, True):
            suffix = '-asan' if sanitized else ''
            binary = out / ('capture-host' + suffix)
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'] if sanitized else []
            run('compile' + suffix, [cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', *flags,
                '-I', str(ROOT / 'package'), str(ROOT / 'scripts/tests/picasso-capture-host.cpp'), '-o', str(binary)])
            run('capture' + suffix, [str(binary)])
        result['passed'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
if __name__ == '__main__':
    main()
