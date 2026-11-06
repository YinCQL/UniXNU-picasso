#!/usr/bin/env python3
"""Regression fixtures for PR18 touch delivery, USB stop and sparse image format."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    extract = load('buttons', ROOT / 'scripts/check-picasso-buttons.py').extract
    source = (ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp').read_text()
    state = source[source.index('    uint32_t lastSeq=0;'):source.index('    // One finger set')]
    send = extract(source, '    void touchSend()').replace('__asm__ volatile("dmb ish":::"memory");', 'host_fence();')
    (out / 'touch.inc').write_text(state + send, newline='\n')
    tick = extract(source, '    static void tick(')
    assert 'if(self->touchRetry && (self->touchEnabled || self->touchOnly))self->touchSend();' in tick
    usb = (ROOT / 'patches/picasso-usb-console-xnu.patch').read_text()
    usb = '\n'.join(line[1:] for line in usb.splitlines() if line.startswith('+') and not line.startswith('+++'))
    (out / 'usb-stop.inc').write_text(extract(usb, 'static void\npicasso_usb_stop_controller(') + '\n' + extract(usb, 'static void\npicasso_usb_disconnect_for_reset('), newline='\n')
    assert extract(usb, 'static void\npicasso_usb_system_reset(').index('picasso_usb_watchdog_terminal();') < extract(usb, 'static void\npicasso_usb_system_reset(').index('picasso_usb_disconnect_for_reset();')
    cc = shutil.which('clang++') or (r'C:/Program Files/LLVM/bin/clang++.exe' if os.name == 'nt' else None)
    runs = []
    for fixture in ('picasso-touch-retry-host.cpp', 'picasso-usb-stop-host.cpp'):
        for sanitized in (False, True):
            name = Path(fixture).stem + ('-sanitized' if sanitized else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            cmd = [cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-I', str(out), str(ROOT / 'scripts/tests' / fixture), '-o', str(exe)]
            if sanitized: cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined']
            for stage, command in (('compile', cmd), ('run', [str(exe)])):
                result = subprocess.run(command, capture_output=True, text=True)
                (out / f'{name}-{stage}.log').write_text(result.stdout + result.stderr)
                if result.returncode: raise RuntimeError(result.stdout + result.stderr)
            runs.append(dict(name=name, passed=True))
    sparse = load('sparse', ROOT / 'scripts/make-android-sparse.py')
    raw, image = out / 'raw.img', out / 'sparse.img'
    for data in (b'', b'a', bytes(8192), bytes(4096) + b'abcd' * 1024 + b'xyz', b'Z' * (sparse.BUFFER + 37)):
        raw.write_bytes(data); sparse.convert(raw, image); assert sparse.check(image, raw)['identical']
    try: sparse.convert(raw, raw)
    except ValueError: pass
    else: raise AssertionError('same-path source truncated')
    data = b'abcd' * 1024 + bytes(4096)
    valid = sparse.HEADER.pack(sparse.SPARSE_MAGIC, 1, 0, 28, 12, 4096, 2, 3, zlib.crc32(data))
    valid += sparse.CHUNK.pack(sparse.FILL, 0, 1, 16) + b'abcd'
    valid += sparse.CHUNK.pack(sparse.DONT_CARE, 0, 1, 12)
    valid += sparse.CHUNK.pack(sparse.CRC32, 0, 0, 16) + struct.pack('<I', zlib.crc32(data))
    image.write_bytes(valid); raw.write_bytes(data); assert sparse.check(image, raw)['identical']
    cases = [valid[:n] for n in range(len(valid))] + [valid + b'x', valid[:-1] + bytes([valid[-1] ^ 1])]
    bad = bytearray(valid); struct.pack_into('<I', bad, 32, 3); cases.append(bad)
    for bad in cases:
        image.write_bytes(bad)
        try: sparse.check(image)
        except ValueError: pass
        else: raise AssertionError('invalid sparse accepted')
    names = ['drivers/NativeHidInput/NativeHidInput.cpp', 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp', 'package/PicassoKeyStore.cpp', 'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp', 'patches/picasso-usb-console-xnu.patch', 'patches/unixnu-source-kernel.patch', 'scripts/build-wf-payload.py', 'scripts/make-android-sparse.py', 'scripts/check-picasso-pr18.py', 'scripts/tests/picasso-touch-retry-host.cpp', 'scripts/tests/picasso-usb-stop-host.cpp']
    (out / 'results.json').write_text(json.dumps(dict(passed=True, runs=runs, malformed_sparse_cases=len(cases), inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}), indent=2)+'\n')
    print('PASS PR18 production touch retry/USB stop and sparse roundtrip/malformed format')

if __name__ == '__main__': main()
