#!/usr/bin/env python3
"""Exercise production GOP import/completion with host IOKit and queue models."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess


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
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    paths = [root / 'drivers/NativeMobileDisplay' / n for n in
             ('NativeGOPPresenter.cpp', 'NativeLegacyDisplay.cpp')]
    presenter, display = [p.read_text() for p in paths]
    methods = '\n'.join(extract(presenter, m).replace('NativeGOPPresenter::', '') for m in
                        ('bool NativeGOPPresenter::offer(', 'IOReturn NativeGOPPresenter::waitSwap(',
                         'IOReturn NativeGOPPresenter::completeSwap(', 'void NativeGOPPresenter::abandonSwap('))
    offer = extract(display, 'if(!gpuBypass && native_legacy_gpu_offer(')
    completion = extract(display, 'void finishGpuSwap(')
    fixture = root / 'scripts/tests/picasso-gop-handoff-host.cpp'
    gate = extract(display, 'void gpuComplete(')
    test = fixture.read_text().replace('// PRESENTER_METHODS', methods).replace('// DISPLAY_OFFER', offer).replace('// DISPLAY_COMPLETION', completion).replace('// DISPLAY_GATE', gate)
    (out / 'handoff.cpp').write_text(test)
    spec = importlib.util.spec_from_file_location('host', root / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars() if os.name == 'nt' else None
    compiler = shutil.which('clang++') or shutil.which('g++')
    report = {'scope': __doc__, 'runs': [], 'inputs': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths + [fixture]}}
    for asan in (False, True):
        label = 'handoff-asan' if asan else 'handoff'
        exe = label + ('.exe' if os.name == 'nt' else '')
        if vcvars:
            flags = '/std:c++17 /EHsc /W4 /WX /Od /Zi' + (' /fsanitize=address' if asan else '')
            command = f'cl /nologo {flags} /Fd:{label}.pdb /Fo:{label}.obj /Fe:{exe} handoff.cpp'
            if asan:
                command += ' /link /INCREMENTAL:NO'
            result = host.batch_run(out, label + '-compile', command + f' > {label}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
        else:
            if not compiler:
                raise RuntimeError('C++ compiler missing')
            command = [compiler, '-std=c++17', '-O0', '-g', '-Wall', '-Wextra', '-Werror', str(out / 'handoff.cpp'), '-o', str(out / exe)]
            if asan:
                command += ['-fsanitize=address', '-fno-omit-frame-pointer']
            result = subprocess.run(command, capture_output=True, text=True)
            (out / (label + '-compile.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((out / (label + '-compile.log')).read_text(errors='replace'))
        result = host.batch_run(out, label + '-run', f'{exe} > {label}-run.log 2>&1\nexit /b %errorlevel%\n', vcvars) if vcvars else subprocess.run([str(out / exe)], capture_output=True, text=True)
        if not vcvars:
            (out / (label + '-run.log')).write_text(result.stdout + result.stderr)
        log = (out / (label + '-run.log')).read_text(errors='replace')
        report['runs'].append({'asan': asan, 'command': command, 'exit': result.returncode, 'log': log})
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(log)
    report['pass'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: GOP import failure recovery, completion ownership and stale callbacks, normal + ASan')


if __name__ == '__main__':
    main()
