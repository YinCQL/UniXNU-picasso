#!/usr/bin/env python3
"""Compile the complete BootState source with host IOKit stand-ins.

Checks behavior and startup failure paths, not SDK/ABI or hardware. Preserves
every result under a new output directory; uses installed tools only.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run_host(out, source, production, fixture, include_name, cases):
    (out / include_name).write_text(production, encoding='utf-8', newline='\n')
    (out / 'host.cpp').write_text(fixture, encoding='utf-8', newline='\n')
    spec = importlib.util.spec_from_file_location('fault_runner', ROOT / 'scripts/check-picasso-user-fault.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    vcvars = runner.find_vcvars() if os.name == 'nt' else None
    cxx = shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not cxx:
        raise SystemExit('An installed MSVC/clang++/g++ compiler is required')
    report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'fixture_sha256': hashlib.sha256(fixture.encode()).hexdigest(), 'runs': [],
              'scope': 'Production service code with host IOKit/atomics; not Apple SDK, Mach-O ABI or device.'}
    try:
        for asan in (False, True):
            name = 'host-asan' if asan else 'host'
            exe = name + ('.exe' if os.name == 'nt' else '')
            log = out / (name + '-compile.log')
            if vcvars:
                command = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi /Fd:{name}.pdb /Fo:{name}.obj /Fe:{exe}'
                if asan:
                    command += ' /fsanitize=address'
                command += ' host.cpp'
                if asan:
                    command += ' /link /INCREMENTAL:NO'
                result = runner.batch_run(out, name + '-compile',
                    f'{command} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0', '-g',
                           'host.cpp', '-o', exe]
                if asan:
                    command.extend(['-fsanitize=address', '-fno-omit-frame-pointer'])
                with log.open('w') as output:
                    result = subprocess.run(command, cwd=out, stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'Compile failed: {log}')
            for case in range(cases):
                log = out / f'{name}-case-{case}.log'
                if vcvars:
                    result = runner.batch_run(out, f'{name}-case-{case}',
                        f'{exe} {case} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
                else:
                    with log.open('w') as output:
                        result = subprocess.run([str(out / exe), str(case)], cwd=out, stdout=output,
                            stderr=subprocess.STDOUT, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
                output = log.read_text(encoding='utf-8', errors='replace')
                report['runs'].append({'asan': asan, 'case': case, 'exit': result.returncode,
                                       'groups': re.findall(r'^PASS: (.+)$', output, re.M)})
                if result.returncode or 'PASS:' not in output:
                    raise RuntimeError(f'Case failed: {log}')
        report['pass'] = True
        print(f'PASS: {cases} scenarios per build; ordinary + ASan')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'package/PicassoBootState.cpp').read_text(encoding='utf-8')
    production = re.sub(r'^#include .*\n', '', source, flags=re.M)
    production = production.replace('\n    __asm__("__GLOBAL__sub_I_PicassoBootState.cpp");', ';')
    if '__asm__' in production:
        raise ValueError('Unexpected initializer label')
    fixture = (ROOT / 'scripts/tests/picasso-boot-state-host.cpp').read_text(encoding='utf-8')
    run_host(out, source, production, fixture, 'boot-state-production.inc', 12)


if __name__ == '__main__':
    main()
