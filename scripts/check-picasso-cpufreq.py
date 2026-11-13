#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production governor with observed low-load and interaction scenarios."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--source', type=Path, default=ROOT/'drivers/NativeCpuFreq/CpuFreqProbe.cpp')
a = p.parse_args(); out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
s = a.source.read_text()
start = s.index('            unsigned want = l.count - 1;')
end = s.index('            khzNs[d] +=', start)
(out/'decision.inc').write_text(s[start:end])
runs = []
for variant, flags in [('normal', []), ('sanitized', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
    exe = out/variant
    command = ['clang++-18', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
               '-I', str(out), str(ROOT/'scripts/tests/picasso-cpufreq-host.cpp'), '-o', str(exe)]
    for kind, cmd in [('compile', command), ('run', [str(exe)])]:
        q = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        (out/(variant+'-'+kind+'.log')).write_text(q.stdout+q.stderr)
        runs.append(dict(name=variant+'-'+kind, command=cmd, exit=q.returncode, output=q.stdout+q.stderr))
        if q.returncode:
            (out/'results.json').write_text(json.dumps(dict(passed=False, runs=runs), indent=2)+'\n')
            raise SystemExit(q.stdout+q.stderr)
(out/'results.json').write_text(json.dumps(dict(passed=True, hardware_tested=False,
    source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(), runs=runs), indent=2)+'\n')
print('PASS production governor: low load, hysteresis, touch, frame floor and LUT bounds; host only')
