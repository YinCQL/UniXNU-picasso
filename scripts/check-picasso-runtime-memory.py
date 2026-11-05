#!/usr/bin/env python3
"""Execute actual DRAM, swap and image-mapping helpers with host API models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^static (?:unsigned|int|void)\n' + name + r'\(.*?^}', source, re.M | re.S)
    if not match:
        raise ValueError('Missing production helper: ' + name)
    return match[0] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--xnu', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = {}
    inputs = {}
    for name in ('normal-dram-io', 'runtime-swap', 'image-mapping'):
        path = ROOT / 'patches' / ('picasso-' + name + '-xnu.patch')
        raw = path.read_bytes()
        inputs[str(path.relative_to(ROOT))] = hashlib.sha256(raw).hexdigest()
        sources[name] = '\n'.join(row[1:] for row in raw.decode().splitlines()
                                  if row.startswith('+') and not row.startswith('+++'))
    helpers = (function(sources['normal-dram-io'], 'picasso_dram_io_wimg') +
               function(sources['runtime-swap'], 'picasso_swap_headroom') +
               function(sources['image-mapping'], 'picasso_image_mapping'))
    if args.xnu:
        real = [(args.xnu / path).read_text(encoding='utf-8') for path in
                ('osfmk/arm/pmap/pmap.c', 'bsd/vm/vm_compressor_backing_file.c', 'bsd/kern/kern_exit.c')]
        for source, name in zip(real, ('picasso_dram_io_wimg', 'picasso_swap_headroom', 'picasso_image_mapping')):
            if function(source, name) not in helpers:
                raise ValueError('Replayed helper differs from tested patch: ' + name)
        assert real[0].count('wimg = picasso_dram_io_wimg(wimg, pa);') == 1
        assert real[1].index('error = picasso_swap_headroom(vp, *size, ctx);') < real[1].index('error = vnode_setsize(vp, *size, IO_NOZEROFILL, ctx);')
        assert 'picasso_image_mapping("fault", far);' in real[2]
        inputs['replayed_sources'] = [hashlib.sha256(s.encode()).hexdigest() for s in real]
    fixture = ROOT / 'scripts/tests/picasso-runtime-memory-host.c'
    shutil.copyfile(fixture, out / 'fixture.c')
    (out / 'runtime-memory.inc').write_text(helpers, encoding='utf-8', newline='\n')
    compiler = os.environ.get('CC') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('clang is required')
    runs = []
    for asan in (False, True):
        name = 'asan' if asan else 'normal'
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', str(out / 'fixture.c'), '-o', str(exe)]
        if asan:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / (name + '-run.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        runs.append(dict(asan=asan, passed=True, output=result.stdout))
    inputs[str(fixture.relative_to(ROOT))] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs, inputs=inputs), indent=2) + '\n')
    print('PASS: actual managed-DRAM policy, swap reserve and mapped-file lifetime; normal + ASan')


if __name__ == '__main__':
    main()
