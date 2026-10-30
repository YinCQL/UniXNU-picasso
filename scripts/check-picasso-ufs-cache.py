#!/usr/bin/env python3
"""Test production UFS window/cache readers with host transport/lock stand-ins.

No Apple SDK, hardware UFS, cryptographic primitive or device qualification.
Preserves normal/ASan compiler and runtime outputs in a new directory.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / 'drivers/NativeUfs/tests'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    vcvars = None
    if os.name == 'nt' and not args.cxx:
        spec = importlib.util.spec_from_file_location('fault', root / 'scripts/check-picasso-user-fault.py')
        fault = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fault)
        vcvars = fault.find_vcvars()
    if not vcvars and not args.cxx:
        parser.error('Requires installed MSVC on Windows or --cxx; no tool is installed')
    report = {'scope': __doc__, 'pass': False, 'runs': [], 'sha256': {}}
    files = [source.parent / 'RootWindowRead.h', source.parent / 'NativeUfs.cpp',
             source / 'RootWindowRead.cpp', source / 'RootVerifiedCache.cpp']
    for p in files:
        report['sha256'][str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()
    try:
        for fixture in ('RootWindowRead', 'RootVerifiedCache'):
            for asan in (False, True):
                name = fixture + ('-asan' if asan else '')
                binary = out / (name + ('.exe' if os.name == 'nt' else ''))
                entry = {'name': name, 'asan': asan}
                report['runs'].append(entry)
                if vcvars:
                    command = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi /Fd:{name}.pdb'
                    if asan:
                        command += ' /fsanitize=address'
                    command += f' /Fo:{name}.obj /Fe:{binary.name} "{source / (fixture + ".cpp")}"'
                    if asan:
                        command += ' /link /INCREMENTAL:NO'
                    entry['command'] = command
                    result = fault.batch_run(out, name + '-compile',
                        f'{command} > {name}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
                else:
                    command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                               str(source / (fixture + '.cpp')), '-o', str(binary)]
                    if asan:
                        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                    entry['command'] = command
                    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
                entry['compile_exit'] = result.returncode
                assert result.returncode == 0, name + ' compile failed'
                if vcvars:
                    result = fault.batch_run(out, name + '-run',
                        f'{binary.name} > {name}-run.log 2>&1\nexit /b %errorlevel%\n', vcvars)
                else:
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
                    (out / (name + '-run.log')).write_text(result.stdout + result.stderr)
                entry['run_exit'] = result.returncode
                assert result.returncode == 0, name + ' run failed'
                print('PASS:', name)
        report['pass'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
