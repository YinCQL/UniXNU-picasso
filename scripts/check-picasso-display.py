#!/usr/bin/env python3
"""Host boundary/ownership tests plus a read-only pinned original ABI audit."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--native-prelink', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    builder = load(ROOT / 'scripts/build-picasso-display.py')
    native = load(args.native_prelink)
    original = args.kernelcache.read_bytes()
    slots, aliases, _ = builder.qualify(native, original)
    for data, slide in ((original, 0), (original[:-1], builder.SLIDE)):
        try:
            builder.qualify(native, data, slide)
        except ValueError:
            pass
        else:
            raise AssertionError('Unaudited original/slide accepted')
    source_dir = ROOT / 'drivers/NativeMobileDisplay'
    template = (source_dir / 'OriginalLegacyFramebufferBase.h.in').read_text()
    queue = extract(template, 'bool activateOnlyPendingSwap(')
    (out / 'PicassoQueueExtracted.h').write_text('struct OriginalLegacySwapRequest;\nstruct QueueFixture {\n'
        'unsigned char storage[0x18a0];\n' + queue + '\n};\n')
    driver = (source_dir / 'NativeLegacyDisplay.cpp').read_text()
    validation = '\n'.join(extract(driver, marker) for marker in
        ('bool isValidated(', 'void removeValidated(', 'bool addValidated('))
    with (out / 'PicassoQueueExtracted.h').open('a') as stream:
        stream.write('struct ValidationFixture {\nconst OriginalLegacySwapRequest *validated[8]={};\n' + validation + '\n};\n')
    report = dict(scope='MSVC host tests of production helpers; original ABI static audit; no Apple SDK or phone execution',
                  original_sha256=hashlib.sha256(original).hexdigest(), virtual_slots=len(slots), aliases=aliases, runs=[])
    host = load(ROOT / 'scripts/check-picasso-user-fault.py')
    vcvars = host.find_vcvars()
    if not vcvars:
        raise RuntimeError('MSVC developer environment missing')
    fixture = ROOT / 'scripts/tests/picasso-display-host.cpp'
    for asan in (False, True):
        label = 'display-asan' if asan else 'display'
        flags = '/std:c++17 /EHsc /W4 /WX /Od' + (' /fsanitize=address /Zi' if asan else '')
        command = f'cl /nologo {flags} /I"{source_dir}" /I"{out}" /Fd:{label}.pdb /Fo:{label}.obj /Fe:{label}.exe "{fixture}"'
        if asan:
            command += ' /link /INCREMENTAL:NO'
        result = host.batch_run(out, label + '-compile', command + f' > {label}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
        if result.returncode:
            raise RuntimeError((out / (label + '-compile.log')).read_text(errors='replace'))
        result = host.batch_run(out, label + '-run', f'{label}.exe > {label}.log 2>&1\nexit /b %errorlevel%\n', vcvars)
        log = (out / (label + '.log')).read_text(errors='replace')
        report['runs'].append(dict(label=label, command=command, exit=result.returncode, log=log))
        if result.returncode:
            raise RuntimeError(log)
        print(label, log.strip())
    # Compile the exact copy helpers for the target, with the production barrier.
    arm = out / 'pixels.cpp'
    arm.write_text('#include "NativeLegacyRequest.h"\nextern "C" int present(volatile void *dst, uint64_t db, uint64_t ds,'
        'const void *src,uint64_t sb,uint64_t ss,uint32_t w,uint32_t h) {\n'
        'int ok=native_legacy_copy_pixels(dst,db,ds,src,sb,ss,w,h);\n'
        'if(ok)__asm__ volatile("dsb sy" ::: "memory");return ok;}\n')
    command = ['C:/Program Files/LLVM/bin/clang++.exe', '--target=arm64-apple-ios17.0', '-std=c++17',
        '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I', str(source_dir), '-c', str(arm), '-o', str(out / 'pixels.o')]
    result = subprocess.run(command, capture_output=True, text=True)
    (out / 'pixels-compile.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr)
    report['arm64_copy_compile'] = command
    inputs = [source_dir / name for name in ('NativeLegacyDisplay.cpp', 'NativeLegacyRequest.h', 'PicassoGop.h', 'OriginalLegacyFramebufferBase.h.in')]
    inputs += [fixture, ROOT / 'scripts/build-picasso-display.py', ROOT / 'package/PicassoDisplay.cpp']
    report['source_sha256'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report['pass'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
