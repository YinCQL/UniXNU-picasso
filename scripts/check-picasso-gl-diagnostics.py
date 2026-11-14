#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise bounded compositor diagnostics from the production source."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source = ROOT / 'drivers/NativeGL/eagl/eagl.c'
fixture = ROOT / 'scripts/tests/picasso-gl-diagnostics-host.c'
s = source.read_text()
body = s[s.index('static int compositor_diagnostics'):s.index('static void\neagl_trace(const char *format, ...)\n{')]
m = re.search(r'static void\ncompositor_draw\(', s)
assert m
body += s[m.start():s.index('\n}', m.start()) + 2] + '\n'
(out / 'diagnostics.inc').write_text(body)
runs = []
for name, flags in [('normal', []), ('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    binary = out / name
    commands = [('compile', ['clang-18', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                            *flags, '-I', str(out), str(fixture), '-o', str(binary)]), ('run', [str(binary)])]
    for step, cmd in commands:
        result = subprocess.run(cmd, text=True, capture_output=True, timeout=60, env=dict(os.environ))
        (out / (name + '-' + step + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        runs.append(dict(name=name + '-' + step, passed=True, output=result.stdout))
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
(out / 'results.json').write_text(json.dumps(dict(passed=True, hardware_tested=False,
    source_sha256=sha(source), inputs={str(p.relative_to(ROOT)): sha(p) for p in (source, fixture, Path(__file__))},
    runs=runs), indent=2) + '\n')
print(runs[-1]['output'])
