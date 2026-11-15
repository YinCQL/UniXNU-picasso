#!/usr/bin/env python3
"""Exercise the production GENI DMA engine with a register and ownership model."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
out = root/'out/geni-test'
out.mkdir(parents=True, exist_ok=True)
sources = ['src/qbt_geni.c', 'tests/geni.c']
command = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-I'+str(root/'include'), '-I'+str(root/'src'),
           *[str(root/s) for s in sources], '-o', str(out/'geni')]
subprocess.run(command, check=True)
r = subprocess.run([out/'geni'], text=True, capture_output=True)
(out/'output.txt').write_text(r.stdout+r.stderr)
if r.returncode:
    raise SystemExit(r.stdout+r.stderr or str(r.returncode))
(out/'report.json').write_text(json.dumps(dict(passed=True, registers_simulated=True,
    hardware_verified=False, inputs={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources},
    output=r.stdout), indent=2)+'\n')
print(r.stdout, end='')
