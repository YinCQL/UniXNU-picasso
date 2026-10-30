#!/usr/bin/env python3
"""Check the actual replayed XNU microtime conversion with host clock stand-ins.

Uses Python's arbitrary-precision whole-timestamp oracle, boundary sweeps and
output canaries. No SDK build, hardware timer measurement or phone access.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (args.xnu / 'osfmk/arm/rtclock.c').read_text(encoding='utf-8')
    blocks = []
    for name in ('absolutetime_to_microtime', 'clock_get_system_microtime'):
        matches = re.findall(r'^void\n' + name + r'\(.*?^}', source, re.M | re.S)
        if len(matches) != 1:
            raise ValueError('Expected one actual function: ' + name)
        blocks.append(matches[0])
    helper = '\n\n'.join(blocks) + '\n'
    if 'rtclock_usec_divisor' in helper:
        raise ValueError('Microtime still uses a truncated ticks-per-usec divisor')
    (out / 'microtime.inc').write_text(helper, encoding='utf-8', newline='\n')

    rng = random.Random(7250)
    vectors = []
    rates = (1, 32768, 500000, 1000000, 19200000, 24000000, 25000000, 4294967295)
    for rate in rates:
        ticks = {0, 1, rate - 1, rate, rate + 1, (1 << 64) - 1}
        for second in (2, 359, 600, 3600, 1 << 32):
            for delta in (-192, -1, 0, 1, 192):
                tick = second * rate + delta
                if 0 <= tick < 1 << 64:
                    ticks.add(tick)
        ticks.update(rng.getrandbits(64) for _ in range(256))
        for tick in sorted(ticks):
            whole = tick * 1000000 // rate
            vectors.append((rate, tick, whole // 1000000, whole % 1000000))
    body = ',\n'.join('{%dU, UINT64_C(%d), UINT64_C(%d), %dU}' % v for v in vectors)
    (out / 'vectors.inc').write_text(body + '\n', encoding='utf-8', newline='\n')
    fixture = (ROOT / 'scripts/tests/picasso-microtime-host.c').read_text(encoding='utf-8')
    (out / 'microtime.c').write_text(fixture, encoding='utf-8', newline='\n')
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars() if os.name == 'nt' and not args.cc else None
    cc = args.cc or shutil.which('clang') or shutil.which('gcc')
    if not vcvars and not cc:
        raise RuntimeError('No installed C compiler; no tools were installed')
    runs = []
    for name, asan in (('microtime', False), ('microtime-asan', True)):
        compile_result, exe = host.compile_case(out, name, 'microtime.c', asan, cc, vcvars)
        if compile_result['exit']:
            raise RuntimeError('Compile failed: ' + compile_result['log'])
        log = out / (name + '-run.log')
        if vcvars:
            result = host.batch_run(out, name + '-run',
                                    f'{exe} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
        else:
            with log.open('w', encoding='utf-8') as output:
                result = subprocess.run([str(out / exe)], cwd=out, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode or 'PASS microtime' not in log.read_text():
            raise RuntimeError('Run failed: ' + str(log))
        runs.append(dict(asan=asan, compile=compile_result, pass_result=True))
    report = dict(scope=__doc__, source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  helper_sha256=hashlib.sha256(helper.encode()).hexdigest(), rates_hz=rates,
                  oracle_vectors=len(vectors), monotonic_sweep_ticks=19200001,
                  runs=runs, pass_result=True)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS microtime: {len(vectors)} oracle vectors, 19.2 MHz monotonic sweep, ordinary + ASan')


if __name__ == '__main__':
    main()
