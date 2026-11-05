#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check production UFS persistence policy and commands with host transport.

Does not write a device or qualify persistent Data, APFS recovery or Apple SDK
compilation. Preserves normal/ASan outputs in a new directory.
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
    native = (source.parent / 'NativeUfs.cpp').read_text(encoding='utf-8')
    begin = '// ---- Persistent Data store window (sheng-data-persist)'
    end = '// Re-read the GPT header and entry array'
    if native.count(begin) != 1 or native.count(end) != 1:
        raise ValueError('Expected one complete board persistence API section')
    actual = native[native.index(begin):native.index(end)]
    (out / 'PersistentBoard.inc').write_text(actual, encoding='utf-8', newline='\n')
    files = [source.parent / 'NativeUfs.cpp', source.parent / 'RootCommands.h',
             source / 'RootCommandsBridge.cpp', source / 'RootCommands.cpp',
             source / 'PersistentWindows.cpp', Path(__file__).resolve()]
    report = {'scope': __doc__, 'pass': False, 'runs': [],
              'sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
              'extracted_sha256': hashlib.sha256(actual.encode()).hexdigest()}
    try:
        for asan in (False, True):
            name = 'persistence' + ('-asan' if asan else '')
            binary = out / (name + ('.exe' if os.name == 'nt' else ''))
            entry = {'name': name, 'asan': asan, 'cases': []}
            report['runs'].append(entry)
            if vcvars:
                command = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi /Fd:{name}.pdb'
                if asan:
                    command += ' /fsanitize=address'
                command += f' /I"{out}" /Fo:{name}.obj /Fe:{binary.name} "{source / "PersistentWindows.cpp"}"'
                if asan:
                    command += ' /link /INCREMENTAL:NO'
                entry['command'] = command
                result = fault.batch_run(out, name + '-compile',
                    f'{command} > {name}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                           '-I', str(out), str(source / 'PersistentWindows.cpp'), '-o', str(binary)]
                if asan:
                    command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                entry['command'] = command
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
            entry['compile_exit'] = result.returncode
            assert result.returncode == 0, name + ' compile failed'
            # Mode caches are production function statics: each setting gets a
            # fresh process. Test default, zero, too small, exact log/export and
            # maximum accepted reserves, both with and without logging.
            for mode in (0, 1, 2):
                for reserve in (0, 128, 288, 1024, 16384):
                    for log in (0, 1):
                        label = f'{name}-{mode}-{reserve}-{log}'
                        if vcvars:
                            result = fault.batch_run(out, label,
                                f'{binary.name} {mode} {reserve} {log} > {label}.log 2>&1\nexit /b %errorlevel%\n', vcvars)
                        else:
                            result = subprocess.run([str(binary), str(mode), str(reserve), str(log)],
                                                    capture_output=True, text=True, timeout=20)
                            (out / (label + '.log')).write_text(result.stdout + result.stderr)
                        entry['cases'].append({'mode': mode, 'reserve_mib': reserve,
                                               'log': log, 'exit': result.returncode})
                        assert result.returncode == 0, label + ' failed'
            print('PASS:', name, '30 mode/reserve/log configurations; six window opening orders each')
        report['pass'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
