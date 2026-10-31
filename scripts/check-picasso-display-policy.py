#!/usr/bin/env python3
"""Exercise the actual display policy with a generated ADT and modeled IOKit.

The fixture uses the production property functions with host stand-ins. It does
not test the DarwinBoot merge, an Apple SDK build, or physical presentation.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    spec = importlib.util.spec_from_file_location('picasso_policy_helper', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    adt = args.adt.read_bytes()
    nodes = load(ROOT / 'scripts/check-picasso-adt.py').decode(adt)
    raw = nodes['/chosen'].get('display-scale')
    if raw is None or len(raw) != 4:
        raise ValueError('ADT must contain one 32-bit chosen display-scale')
    scale = struct.unpack('<I', raw)[0]
    source = (ROOT / 'package/PicassoDisplay.cpp').read_text(encoding='utf-8')
    markers = ['static void finishDisplayPolicy(', 'static bool matchWord(']
    if 'static bool matchDisplayScale(' in source:
        markers.append('static bool matchDisplayScale(')
    markers.append('extern "C" bool native_legacy_gop_mapping_observed(')
    actual = '\n\n'.join(extract(source, marker) for marker in markers) + '\n'
    (out / 'display-policy.inc').write_text(actual, encoding='utf-8', newline='\n')
    fixture = ROOT / 'scripts/tests/picasso-display-policy-host.cpp'
    host = load(ROOT / 'scripts/check-picasso-user-fault.py')
    vcvars = host.find_vcvars() if os.name == 'nt' else None
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not compiler:
        raise RuntimeError('No C++ compiler available')
    report = {'scope': __doc__, 'adt_sha256': hashlib.sha256(adt).hexdigest(),
              'adt_scale': scale, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'runs': [], 'pass': False}
    try:
        for asan in (False, True):
            label = 'display-policy-asan' if asan else 'display-policy'
            compile_log, run_log = out / (label + '-compile.log'), out / (label + '-run.log')
            exe = label + ('.exe' if os.name == 'nt' else '')
            if vcvars:
                flags = '/std:c++17 /EHsc /W4 /WX /Od /Zi' + (' /fsanitize=address' if asan else '')
                command = f'cl /nologo {flags} /I"{out}" /Fd:{label}.pdb /Fo:{label}.obj /Fe:{exe} "{fixture}"'
                if asan:
                    command += ' /link /INCREMENTAL:NO'
                result = host.batch_run(out, label + '-compile', command + f' > {compile_log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0', '-g',
                           '-I', str(out), str(fixture), '-o', str(out / exe)]
                if asan:
                    command.insert(1, '-fsanitize=address')
                with compile_log.open('w', encoding='utf-8') as log:
                    result = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
            run = {'asan': asan, 'compile_exit': result.returncode, 'compile_log': str(compile_log)}
            report['runs'].append(run)
            if result.returncode:
                raise RuntimeError('Display policy compilation failed: ' + str(compile_log))
            if vcvars:
                result = host.batch_run(out, label + '-run', f'{exe} {scale} > {run_log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                with run_log.open('w', encoding='utf-8') as log:
                    result = subprocess.run([str(out / exe), str(scale)], cwd=out, stdout=log, stderr=subprocess.STDOUT)
            run.update(run_exit=result.returncode, run_log=str(run_log))
            if result.returncode or 'PASS display policy:' not in run_log.read_text(encoding='utf-8', errors='replace'):
                raise RuntimeError('Display policy test failed: ' + str(run_log))
        report['pass'] = True
        print(f'PASS display policy: actual ADT scale={scale}, normal and ASan')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
