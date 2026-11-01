#!/usr/bin/env python3
"""Exercise replayed iOS 16 framebuffer/IOSurface conversions and dispatch.

The actual helpers and callExternalMethod are compiled with host IOKit and
allocation stand-ins. This checks bounds and restoration, not stock framework
ABI qualification, Apple SDK compilation or physical presentation.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (args.xnu / 'iokit/Kernel/IOUserClient.cpp').read_text()
    methods = re.findall(r'IOReturn\nIOUserClient::callExternalMethod\(.*?^}', source, re.M | re.S)
    if len(methods) != 1:
        raise ValueError('Expected one dispatch function')
    method = re.sub(r'/\* PICASSO_FB_METHOD_HOOK_BEGIN \*/.*?/\* PICASSO_FB_METHOD_HOOK_END \*/\n', '', methods[0], flags=re.S)
    start = source.rfind('#if DEVELOPMENT || DEBUG', 0, source.index('/* Picasso runs iOS 16 IOMobileFramebuffer'))
    helpers = source[start:source.index(methods[0])]
    actual = helpers + method + '\n'
    (out / 'ios16-surface-abi.inc').write_text(actual, encoding='utf-8', newline='\n')
    fixture = ROOT / 'scripts/tests/picasso-ios16-surface-abi-host.cpp'
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars() if os.name == 'nt' else None
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not compiler:
        raise RuntimeError('No C++ compiler available')
    report = {'scope': __doc__, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'actual_sha256': hashlib.sha256(actual.encode()).hexdigest(), 'runs': [], 'pass': False}
    try:
        for asan in (False, True):
            label = 'ios16-abi-asan' if asan else 'ios16-abi'
            exe = label + ('.exe' if os.name == 'nt' else '')
            compile_log, run_log = out / (label + '-compile.log'), out / (label + '-run.log')
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
                with compile_log.open('w') as log:
                    result = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
            run = {'asan': asan, 'compile_exit': result.returncode}
            report['runs'].append(run)
            if result.returncode:
                raise RuntimeError('Compilation failed: ' + str(compile_log))
            if vcvars:
                result = host.batch_run(out, label + '-run', f'{exe} > {run_log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                with run_log.open('w') as log:
                    result = subprocess.run([str(out / exe)], cwd=out, stdout=log, stderr=subprocess.STDOUT)
            run['run_exit'] = result.returncode
            if result.returncode or 'PASS iOS16 ABI:' not in run_log.read_text(errors='replace'):
                raise RuntimeError('ABI check failed: ' + str(run_log))
        report['pass'] = True
        print('PASS actual iOS 16 framebuffer/IOSurface conversions and dispatch, normal and ASan')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
