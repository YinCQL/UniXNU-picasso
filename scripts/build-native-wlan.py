#!/usr/bin/env python3
"""Build the picasso (SM7250 / WCN3998) WLAN bring-up probe for the XNU kernel.

Compiles the board facts, the pure MBN/WLFW parsers and the staged IOKit probe with
this tree's recorded XNU kext flags.

Output: build/native-wlan/libNativeWlan.a, force-loaded by
scripts/build-xnu-4k-baseline.sh when CONFIG_XNU_NATIVE_WLAN=1. The probe is inert
without a `sheng-wlan...` boot argument, so an image carrying it behaves as before.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

DRIVER = ROOT / 'drivers/NativeWlan'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
OUT = ROOT / 'build/native-wlan'
OUT.mkdir(parents=True, exist_ok=True)


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', default=str(ROOT / RECORD))
    args = parser.parse_args()

    objects = []
    for name in ('WlanFirmware.c',):
        obj = OUT / (name + '.o')
        compile_one(args.record, DRIVER / name, obj, 'c')
        objects.append(obj)
    for name in ('WlanBoard.cpp', 'WlanProbe.cpp'):
        obj = OUT / (name + '.o')
        compile_one(args.record, DRIVER / name, obj, 'c++')
        objects.append(obj)

    archive = OUT / 'libNativeWlan.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, *objects])
    print('native-wlan:', archive)


if __name__ == '__main__':
    main()
