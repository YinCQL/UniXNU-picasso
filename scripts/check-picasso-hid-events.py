#!/usr/bin/env python3
"""Exercise actual HID wrapper with model objects, bounded tables and ASan; not a donor ABI check."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('buttons', ROOT / 'scripts/check-picasso-buttons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = (ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp').read_text()
    start = source.index('enum {kSlots=256')
    setup = source[start:source.index('#if __has_feature(ptrauth_calls)', start)]
    call = module.extract(source, 'template <typename R,typename... A> static R call(void *object,unsigned slot,uint16_t,A... args)')
    mark = module.extract(source, 'static bool mark(')
    dispatch = module.extract(source, 'static void dispatch(')
    install = module.extract(source[source.index('#include "OriginalHidEventABI.h"'):], 'static void install(')
    install = install.replace('__asm__ volatile("dmb ish":::"memory");', '__atomic_thread_fence(__ATOMIC_RELEASE);')
    # Match production conditional compilation; the host has no PAC.
    dispatch = dispatch.replace('#if __has_feature(ptrauth_calls)\n    originalDispatch(self,event,options);\n#else\n', '').replace('#endif\n', '')
    (out / 'hid-events.inc').write_text(setup + call + '\n' + mark + '\n' + dispatch + '\n' + install, newline='\n')
    cc = shutil.which('clang++') or (r'C:\Program Files\LLVM\bin\clang++.exe' if os.name == 'nt' else None)
    if not cc:
        raise RuntimeError('clang++ required')
    runs = []
    for sanitized in (False, True):
        name = 'events' + ('-sanitized' if sanitized else '')
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        cmd = [cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-I', str(out),
               str(ROOT / 'scripts/tests/picasso-hid-events-host.cpp'), '-o', str(exe)]
        if sanitized:
            cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined']
        for stage, command in (('compile', cmd), ('run', [str(exe)])):
            result = subprocess.run(command, capture_output=True, text=True)
            (out / (name + '-' + stage + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            runs.append(dict(name=name, stage=stage, passed=True))
    (out / 'results.json').write_text(json.dumps(dict(scope=__doc__, passed=True, runs=runs,
        source_sha256=hashlib.sha256(source.encode()).hexdigest()), indent=2) + '\n')
    print('PASS HID table bounds/rejection, unchanged Consumer dispatch, logging limit and digitizer edge state')


if __name__ == '__main__':
    main()
