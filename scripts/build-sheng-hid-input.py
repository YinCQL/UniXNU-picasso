#!/usr/bin/env python3
"""Build the owned offline HID source (ShengHidInput) and its kernel probe.

The device object is qualified against the ORIGINAL IOHIDDevice vtable in the
hash-pinned kernelcache (scripts/build-native-hid-input.py): the instruction
identity, the 207-slot vtable and the C2/D2 bindings must match the pinned
IOHIDFamily, so the ABI header is generated, never hand-written. The probe
(package/HidInputProbe.cpp) owns the kext load and the device attach.

Output: build/sheng-hid-input/libShengHidInput.a, force-loaded by
scripts/build-xnu-4k-baseline.sh when CONFIG_XNU_SHENG_HIDINPUT=1.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
HID_OUT = ROOT / 'build/native-hid-input'
OUT = ROOT / 'build/sheng-hid-input'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, cwd=None):
    print('+', ' '.join(map(str, command)))
    subprocess.run([str(value) for value in command], check=True, timeout=600, cwd=cwd)


def compile_cpp(record_path, source, obj):
    info = json.loads(Path(record_path).read_text().strip().rstrip(','))
    command = []
    arguments = info['arguments']
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == info['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(obj)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ', '--serialize-diagnostics'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend([
        '-Wall', '-Wextra', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        '-Wno-unknown-warning-option', '-Wno-missing-designated-field-initializers',
        '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(source.parent),
        '-I', str(ROOT / 'drivers/NativeNovatekTouch'),
    ])
    # The recorded command starts with -include meta_features.h, which lives in
    # the record's own build directory.
    run(command, cwd=info['directory'])


# The ABI header and the qualified objects come from the reviewable builder.
run(['python3', ROOT / 'scripts/build-native-hid-input.py'])
for name in ('NativeHidInput.late.o', 'hid-original-bindings.o'):
    if not (HID_OUT / name).is_file():
        raise SystemExit(f'missing qualified object: {HID_OUT / name}')

probe = OUT / 'HidInputProbe.o'
# The assembled tree's package/ is the upstream package module; the device package is kits/<kit>. Prefer the kit
# source so a re-prepared tree never compiles the stale copy (or the upstream package without a probe at all).
probe_source = ROOT / 'kits/picasso/HidInputProbe.cpp'
if not probe_source.is_file():
    probe_source = ROOT / 'package/HidInputProbe.cpp'
compile_cpp(ROOT / RECORD, probe_source, probe)

archive = OUT / 'libShengHidInput.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive,
     HID_OUT / 'NativeHidInput.late.o', HID_OUT / 'hid-original-bindings.o', probe])
print(f'sheng-hid-input: {archive}')
