#!/usr/bin/env python3
"""Build the Novatek NT36672C touch driver for picasso and its firmware image.

Compiles the vendored GENI SPI engine, the NT36xxx protocol, the 18x36 contact
core and the staged probe with this tree's recorded XNU kext flags, then turns
the phone's authenticated stock firmware into a linked array for the probe.

Output: build/novatek-touch/libNativeNovatekTouch.a, selected by board.toml.

Firmware: --firmware PATH, or NOVATEK_TOUCH_FW. Without either, build read-only
probe support; stage 4 still requires firmware and fails explicitly without it.
Nothing is written back to the repository; the blob stays at the caller's path.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
from pathlib import Path
import picasso_touch_firmware as firmware

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'
# The pinned stock image queued by scripts/fetch-picasso-touch-firmware.py. Defaulting
# to it keeps stage 4 in every plain kernel rebuild: without this, a rebuild that did
# not export NOVATEK_TOUCH_FW silently regenerated the read-only archive and the next
# boot lost the physical touch panel. The prepared tree holds copies of scripts/, so
# search the parents too and find the worktree's out/ rather than out/picasso/tree/out/.
def _default_firmware(root):
    for candidate in (root, *root.parents):
        path = candidate / 'out/picasso/novatek/firmware.bin'
        if path.is_file():
            return path
    return root / 'out/picasso/novatek/firmware.bin'


DEFAULT_FIRMWARE = _default_firmware(REPO)

DRIVER = ROOT / 'drivers/NativeNovatekTouch'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
OUT = ROOT / 'build/novatek-touch'


def run(command, cwd=None):
    print('+', ' '.join(map(str, command)))
    subprocess.run([str(value) for value in command], check=True, timeout=900, cwd=cwd)


def flags(record_path, source, obj, language):
    """Recorded IOPlatformExpert compile line, retargeted to our source.

    C++: keep the recorded language. C: swap -xc++/-std=gnu++2b for C and drop the
    C++-only vendor features (the -Xclang pairs) instead of enumerating them.
    """
    info = json.loads(Path(record_path).read_text().strip().rstrip(','))
    command, index, arguments = [], 0, info['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == info['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(obj)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ', '--serialize-diagnostics'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP', '--driver-mode=g++', '-fno-odr-hash-protocols',
                       '-fno-c++-static-destructors'):
            pass
        elif value == '-xc++':
            command.append('-xc++' if language == 'c++' else '-xc')
        elif value == '-std=gnu++2b':
            command.append('-std=gnu++2b' if language == 'c++' else '-std=gnu11')
        elif value == '-Xclang':
            index += 1
        elif value.startswith('-W'):
            pass
        else:
            command.append(value)
        index += 1
    command.extend(['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unknown-warning-option',
                    '-Wno-missing-designated-field-initializers',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(source.parent),
                    '-I', str(DRIVER)])
    return command, info['directory']


def compile_one(record_path, source, obj, language):
    command, cwd = flags(record_path, source, obj, language)
    run(command, cwd=cwd)


def inspect_objects(objects):
    def load(path):
        spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    platform = load(ROOT / 'scripts/build-picasso-pmgr.py')
    native = load(ROOT / 'kits/XnuPrelink/native_prelink.py')
    reports = {}
    for obj in objects:
        _, sections, symbols = platform.inspect_object(obj, native)
        platform.require(not any(s['name'] == '__mod_init_func' for s in sections),
                         'Touch archive must have no automatic initializer')
        if obj.name == 'TouchProbe.o':
            for name in ('seq', 'count', 'writer_lock', 'xs', 'ys', 'ids'):
                matches = [s for s in symbols if s['name'] == '_native_touch_' + name]
                platform.require(len(matches) == 1 and matches[0]['kind'] == 1,
                                 'HID must own the shared touch storage: ' + name)
        reports[obj.name] = hashlib.sha256(obj.read_bytes()).hexdigest()
    return reports


def read_firmware(path):
    if path is None:
        return b''
    if not path.is_file():
        raise SystemExit(f'touch firmware not found: {path} (set NOVATEK_TOUCH_FW or --firmware)')
    data = path.read_bytes()
    firmware.verify(data)
    return data


def emit_firmware(data, out_c):
    lines = ['/* Generated by scripts/build-native-novatek-touch.py; not committed. */',
             '#include <stdint.h>',
             'const uint8_t native_novatek_touch_firmware[] = {']
    for start in range(0, len(data), 16):
        chunk = data[start:start + 16]
        lines.append('    ' + ','.join(f'0x{b:02x}' for b in chunk) + ',')
    if not data:
        lines.append('    0, /* No firmware supplied; stage 4 cannot run. */')
    lines += ['};', f'const uint32_t native_novatek_touch_firmware_size = {len(data)}u;', '']
    out_c.write_text('\n'.join(lines))
    return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path,
                        default=os.environ.get('NOVATEK_TOUCH_FW') or None,
                        help='SHA256-pinned 128 KiB Novatek stock image')
    parser.add_argument('--record', default=str(ROOT / RECORD))
    args = parser.parse_args()
    path = args.firmware
    if path is None and DEFAULT_FIRMWARE.is_file():
        path = DEFAULT_FIRMWARE
    data = read_firmware(path)
    OUT.mkdir(parents=True, exist_ok=True)

    objects = []
    for name in ('GeniSpi.c', 'NvtProtocol.c', 'NvtTouchCore.c'):
        obj = OUT / (name + '.o')
        compile_one(args.record, DRIVER / name, obj, 'c')
        objects.append(obj)
    probe = OUT / 'TouchProbe.o'
    compile_one(args.record, DRIVER / 'TouchProbe.cpp', probe, 'c++')
    objects.append(probe)

    fw_c = OUT / 'novatek_firmware.c'
    size = emit_firmware(data, fw_c)
    fw_obj = OUT / 'novatek_firmware.o'
    compile_one(args.record, fw_c, fw_obj, 'c')
    objects.append(fw_obj)
    object_hashes = inspect_objects(objects)
    print(f'novatek-touch: firmware {path} ({size} bytes)')
    if not size:
        print('novatek-touch: read-only probe build; stage 4 requires --firmware or NOVATEK_TOUCH_FW')

    archive = OUT / 'libNativeNovatekTouch.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, *objects])
    print('novatek-touch:', archive)
    report = dict(complete=True, firmware=firmware.verify(data) if data else None,
                  objects=object_hashes,
                  archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                  sources={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted(DRIVER.glob('*')) if path.is_file()},
                  hardware_tested=False)
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
