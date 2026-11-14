#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check scene snapshot resampling and the production IOSurface transaction."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
sources = [ROOT / 'package/picasso-springboard' / name for name in
           ['picasso-snapshot-resample-core.h', 'picasso-snapshot-surface.h']]
runs = []
for test in ['resample', 'surface']:
    source = ROOT / ('scripts/tests/picasso-snapshot-' + test + '-host.c')
    sources.append(source)
    for variant, flags in [('normal', ['-O2']), ('sanitizer', ['-O1', '-g',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        name = test + '-' + variant
        exe = out / name
        command = ['clang-18', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                   '-I', str(ROOT / 'package/picasso-springboard'), str(source), '-lm', '-o', str(exe)]
        for step, cmd in [('compile', command), ('run', [str(exe)])]:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            (out / (name + '-' + step + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise SystemExit(result.stdout + result.stderr)
            runs.append(dict(name=name+'-'+step, passed=True, command=cmd, output=result.stdout))
        print(name, 'passed', flush=True)
(out / 'results.json').write_text(json.dumps(dict(passed=True, hardware_tested=False,
    scope=__doc__, sources={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sources}, runs=runs), indent=2) + '\n')
