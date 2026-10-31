#!/usr/bin/env python3
"""Test replayed iOS 16 registered-port adapters with host IPC stand-ins.

Executes the real adapters and the unchanged fixed-three XNU helpers. Locks,
allocation, reference primitives and child creation are modeled; this is not
a MIG server build or device acceptance test.
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
    parser.add_argument('--cc')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (args.xnu / 'osfmk/kern/ipc_tt.c').read_text(encoding='utf-8')
    defs = (args.xnu / 'osfmk/mach/task.defs').read_text(encoding='utf-8')
    names = ('_kernelrpc_mach_ports_register3', '_kernelrpc_mach_ports_lookup3',
             'mach_ports_register', 'mach_ports_lookup')
    blocks = []
    for name in names:
        matches = re.findall(r'^kern_return_t\n' + name + r'\(.*?^}', source, re.M | re.S)
        if len(matches) != 1:
            raise ValueError('Expected one actual function: ' + name)
        blocks.append(matches[0])
    # Both directions must retain the original OOL-array wire declaration.
    for name in names[2:]:
        match = re.search(r'routine\s+' + name + r'\(.*?\);', defs, re.S)
        if not match or 'target_task\t: task_t;' not in match[0] or \
                '^array[] of mach_port_t' not in match[0]:
            raise ValueError('Missing task-control/OOL-array declaration: ' + name)
    if any(name in defs for name in names[:2]):
        raise ValueError('The Picasso task RPC still selects the fixed-three wire ABI')
    helper = '\n\n'.join(blocks) + '\n'
    (out / 'mach-ports.inc').write_text(helper, encoding='utf-8', newline='\n')
    shutil.copyfile(ROOT / 'scripts/tests/picasso-mach-ports-host.c', out / 'mach-ports.c')
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars() if os.name == 'nt' and not args.cc else None
    cc = args.cc or shutil.which('clang') or shutil.which('gcc')
    if not vcvars and not cc:
        raise RuntimeError('No installed C compiler')
    report = dict(scope=__doc__, source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  helper_sha256=hashlib.sha256(helper.encode()).hexdigest(), runs=[], pass_result=False)
    try:
        for name, asan in (('mach-ports', False), ('mach-ports-asan', True)):
            compiled, exe = host.compile_case(out, name, 'mach-ports.c', asan, cc, vcvars)
            report['runs'].append(dict(asan=asan, compile=compiled))
            if compiled['exit']:
                raise RuntimeError('Compile failed: ' + compiled['log'])
            log = out / (name + '-run.log')
            if vcvars:
                result = host.batch_run(out, name + '-run',
                                        f'{exe} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                with log.open('w', encoding='utf-8') as output:
                    result = subprocess.run([str(out / exe)], cwd=out, stdout=output, stderr=subprocess.STDOUT)
            if result.returncode or 'PASS mach ports: 265 cases' not in log.read_text():
                raise RuntimeError('Run failed: ' + str(log))
            report['runs'][-1]['pass_result'] = True
        report['pass_result'] = True
        print('PASS mach ports: 265 normal + 265 ASan contract cases')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
