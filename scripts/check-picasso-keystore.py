#!/usr/bin/env python3
"""Exercise the real state-only service with host IOKit stand-ins, retaining results."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'IOKit').mkdir()
    for name in ('IOService', 'IOUserClient', 'IOMemoryDescriptor', 'IOLib'):
        (out / 'IOKit' / (name + '.h')).write_text('// API supplied by the host fixture.\n')
    source = ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
    logger_source = (ROOT / 'package/PicassoKeyStore.cpp').read_text()
    logger = logger_source.split('extern "C" void picasso_aks_observed(', 1)[1].split('extern "C" void picasso_keystore_probe', 1)[0]
    logger = 'extern "C" void picasso_aks_observed(' + logger
    (out / 'observed.inc').write_text(logger)
    fixture = ROOT / 'scripts/tests/picasso-keystore-host.cpp'
    text = fixture.read_text()
    # Windows atomics use MSVC intrinsics. Clang has the same production builtins.
    if os.name != 'nt':
        start = text.index('#include <intrin.h>')
        end = text.index('using UInt32=', start)
        text = text[:start] + text[end:]
    (out / 'test.cpp').write_text(text)
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
    report = {'scope': __doc__, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'logger_sha256': hashlib.sha256(logger.encode()).hexdigest(), 'runs': []}
    for asan in (False, True):
        name = 'state-asan' if asan else 'state'
        if os.name == 'nt':
            vcvars = host.find_vcvars()
            flags = ' /fsanitize=address' if asan else ''
            cmd = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi{flags} /I"{out}" /I"{source.parent}" /Fd:{name}.pdb /Fo:{name}.obj /Fe:{name}.exe test.cpp /link /INCREMENTAL:NO'
            result = host.batch_run(out, name, cmd + f' > {name}-compile.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n{name}.exe > {name}.log 2>&1\nexit /b %errorlevel%\n', vcvars)
        else:
            cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-I', str(out), '-I', str(source.parent), str(out / 'test.cpp'), '-o', str(out / name)]
            if asan: cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            result = subprocess.run(cmd, capture_output=True, text=True)
            (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
            if not result.returncode:
                result = subprocess.run([str(out / name)], capture_output=True, text=True)
                (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'State service check failed; preserved at {out}')
        log = (out / (name + '.log')).read_text()
        assert log.startswith('PASS ')
        report['runs'].append({'asan': asan, 'pass': True, 'log': log})
    report['pass'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: state-only service, ordinary and sanitizer host tests')


if __name__ == '__main__':
    main()
