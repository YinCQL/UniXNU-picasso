#!/usr/bin/env python3
"""Exercise the replayed display-only RPC trace with host stand-ins.

The fixture does not run the kernel, MIG, the framebuffer driver or a device.
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
CASES = 4


def load(path):
    spec = importlib.util.spec_from_file_location('picasso_host', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (args.xnu / 'iokit/Kernel/IOUserClient.cpp').read_text(encoding='utf-8')
    blocks = re.findall(r'/\* PICASSO_FB_METHOD_TRACE_BEGIN \*/.*?/\* PICASSO_FB_METHOD_TRACE_END \*/', source, re.S)
    if len(blocks) != 1:
        raise ValueError('Expected one framebuffer method trace')
    functions = re.findall(r'IOReturn\nIOUserClient::callExternalMethod\(.*?^}', source, re.M | re.S)
    if len(functions) != 1:
        raise ValueError('Expected one original dispatch function')
    method = functions[0]
    hooks = re.findall(r'/\* PICASSO_FB_METHOD_HOOK_BEGIN \*/.*?/\* PICASSO_FB_METHOD_HOOK_END \*/\n', method, re.S)
    if len(hooks) != 2 or method.index('picasso_fb_method_begin') > method.index('ipcEnter(') or \
            method.index('picasso_fb_method_end') < method.index('ipcExit('):
        raise ValueError('Missing or misplaced trace hooks')
    clean = re.sub(r'/\* PICASSO_FB_METHOD_HOOK_BEGIN \*/.*?/\* PICASSO_FB_METHOD_HOOK_END \*/\n', '', method, flags=re.S)
    expected = '''IOReturn
IOUserClient::callExternalMethod(uint32_t selector, IOExternalMethodArguments * args)
{
\tIOReturn ret;

\tipcEnter(defaultLocking ? (defaultLockingSingleThreadExternalMethod ? kIPCLockWrite : kIPCLockRead) : kIPCLockNone);
\tif (uc2022) {
\t\tret = ((IOUserClient2022 *) this)->externalMethod(selector, (IOExternalMethodArgumentsOpaque *) args);
\t} else {
\t\tret = externalMethod(selector, args);
\t}
\tipcExit(defaultLocking ? (defaultLockingSingleThreadExternalMethod ? kIPCLockWrite : kIPCLockRead) : kIPCLockNone);

\treturn ret;
}'''
    if clean != expected:
        raise ValueError('Original external-method dispatch changed')
    (out / 'fb-method-trace.inc').write_text(blocks[0] + '\n', encoding='utf-8', newline='\n')
    fixture = ROOT / 'scripts/tests/picasso-fb-method-trace-host.cpp'
    host = load(ROOT / 'scripts/check-picasso-user-fault.py')
    vcvars = host.find_vcvars() if os.name == 'nt' else None
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not compiler:
        raise RuntimeError('No C++ compiler available')
    report = {'scope': __doc__, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'helper_sha256': hashlib.sha256(blocks[0].encode()).hexdigest(),
              'original_dispatch_unchanged': True, 'runs': [], 'pass': False}
    try:
        for asan in (False, True):
            label = 'fb-trace-asan' if asan else 'fb-trace'
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
                           '-pthread', '-I', str(out), str(fixture), '-o', str(out / exe)]
                if asan:
                    command.insert(1, '-fsanitize=address')
                with compile_log.open('w', encoding='utf-8') as log:
                    result = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
            run = {'asan': asan, 'compile_exit': result.returncode, 'compile_log': str(compile_log)}
            report['runs'].append(run)
            if result.returncode:
                raise RuntimeError('Compilation failed: ' + str(compile_log))
            if vcvars:
                commands = ''.join(f'{exe} {case} >> {run_log.name} 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n' for case in range(CASES))
                result = host.batch_run(out, label + '-run', commands + 'exit /b 0\n', vcvars)
            else:
                result = None
                with run_log.open('w', encoding='utf-8') as log:
                    for case in range(CASES):
                        result = subprocess.run([str(out / exe), str(case)], cwd=out, stdout=log, stderr=subprocess.STDOUT)
                        if result.returncode:
                            break
            text = run_log.read_text(encoding='utf-8', errors='replace')
            passed = list(map(int, re.findall(r'^PASS scenario=(\d+)\s*$', text, re.M)))
            run.update(run_exit=result.returncode, run_log=str(run_log), cases=passed)
            if result.returncode or passed != list(range(CASES)):
                raise RuntimeError('Trace test failed: ' + str(run_log))
        report['pass'] = True
        print(f'PASS framebuffer method trace: {CASES} normal + {CASES} ASan scenarios')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
