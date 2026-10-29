#!/usr/bin/env python3
"""Check production watchdog lifecycle ordering with host IOKit/policy stand-ins.

Extracts the native class and unchanged lifecycle/client methods. Original kext
loading and metaclass enumeration are checked by the builder/runtime guards,
not this host fixture. No SDK or device validation is implied.
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


def method(source, name):
    start = re.search(r'^\S[^\n]*PicassoWatchdogDriver::' + name + r'\(', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--xnu', required=True, type=Path, help='Fixed XNU source with the actual IOWorkLoop header')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    header = (args.xnu / 'iokit/IOKit/IOWorkLoop.h').read_text(encoding='utf-8')
    clean = re.sub(r'/\*.*?\*/|//[^\n]*', '', header, flags=re.S)
    patterns = [(r'typedef IOReturn \(\*Action\)\([^;]+;', 'public'),
                (r'virtual IOReturn runAction\([^;]+;', 'public'),
                (r'virtual void openGate\(\);', 'protected'),
                (r'virtual void closeGate\(\);', 'protected')]
    declarations = []
    for pattern, required in patterns:
        match = re.search(pattern, clean)
        if match is None:
            raise ValueError(f'Missing fixed-header declaration: {pattern}')
        access = re.findall(r'\b(public|protected|private)\s*:', clean[:match.start()])[-1]
        if access != required:
            raise ValueError(f'Unexpected access for {pattern}: {access}')
        declarations.append(access + ':\n' + match[0])
    (out / 'workloop-access.inc').write_text('\n'.join(declarations) + '\n', encoding='utf-8')
    source = (ROOT / 'package/PicassoWatchdog.cpp').read_text(encoding='utf-8')
    production = source[source.index('class PicassoWatchdogDriver'):source.index('// Only the native subclass')]
    for name in ('getWorkLoop', 'newUserClient', 'scheduleNextCheck', 'enableWatchdogHardware',
                 'disableWatchdogHardware', 'petWatchdogHardware', 'timerFired', 'start', 'free'):
        production += method(source, name)
    fixture = (ROOT / 'scripts/tests/picasso-watchdog-service-host.cpp').read_text(encoding='utf-8')
    spec = importlib.util.spec_from_file_location('platform_host', ROOT / 'scripts/check-picasso-boot-state.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.run_host(out, source, production, fixture, 'watchdog-lifecycle-production.inc', 10)
    # The old direct-access pattern must now be rejected by the compiler, using
    # access specifiers from the real XNU header rather than a permissive mock.
    report_path = out / 'results.json'
    report = json.loads(report_path.read_text(encoding='utf-8'))
    report['pass'] = False
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    spec = importlib.util.spec_from_file_location('fault_runner', ROOT / 'scripts/check-picasso-user-fault.py')
    compiler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compiler)
    vcvars = compiler.find_vcvars() if os.name == 'nt' else None
    log = out / 'protected-negative.log'
    if vcvars:
        result = compiler.batch_run(out, 'protected-negative',
            'cl /nologo /std:c++17 /EHsc /W4 /WX /Zs /DTEST_ILLEGAL_WORKLOOP_ACCESS host.cpp '
            '> protected-negative.log 2>&1\nexit /b %errorlevel%\n', vcvars)
    else:
        with log.open('w') as output:
            result = subprocess.run([shutil.which('clang++') or shutil.which('g++'), '-std=c++17',
                '-fsyntax-only', '-DTEST_ILLEGAL_WORKLOOP_ACCESS', 'host.cpp'], cwd=out,
                stdout=output, stderr=subprocess.STDOUT)
    text = log.read_text(encoding='utf-8', errors='replace')
    if result.returncode == 0 or not any(mark in text for mark in ('C2248', 'protected member', 'is protected')):
        raise RuntimeError(f'Expected protected-access compile rejection: {log}')
    report.update(workloop_header_sha256=hashlib.sha256(header.encode()).hexdigest(),
                  actual_header_access_verified=True, protected_access_negative_exit=result.returncode)
    report['pass'] = True
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: actual XNU header access and direct-access compile rejection')


if __name__ == '__main__':
    main()
