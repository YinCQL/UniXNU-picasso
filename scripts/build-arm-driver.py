#!/usr/bin/env python3
"""Compile a platform archive using the actual Darwin 23 XNU compile record.

Used inside the assembled UniXNU tree. Runs the recorded compiler and xcrun
ar; writes only build/<component>/. Does not access a device.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'


def compile_file(source: Path, destination: Path, record: dict, extra_flags: list, is_c: bool = False):
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = []
    arguments = record['arguments']
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if is_c and value == '-xc++':
            command.append('-xc')
        elif is_c and value == '-fapple-kext':
            pass
        elif value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if is_c:
        command.append('-std=gnu11')
    command.extend(['-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-c99-designator', '-Wno-reorder-init-list',
                    '-Wno-deprecated-enum-enum-conversion',
                    '-Wno-unnecessary-virtual-specifier',
                    '-Wno-unused-variable', '-Wno-unused-private-field',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk')])
    command.extend(extra_flags)
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'Failed to compile {source}:\n' + result.stdout + result.stderr)
    data = destination.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'Expected a plain ARM64 Mach-O object for {destination}')
    offset = 32
    deployment = None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, length = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit(f'Expected iOS 17 deployment for {destination}, got {deployment}')
    return destination


def build_adreno(record: dict):
    out = ROOT / 'build/native-adreno'
    out.mkdir(parents=True, exist_ok=True)
    extra_flags = [
        '-I', str(ROOT / 'drivers/NativeAdreno'),
        '-I', str(ROOT / 'kits/AdrenoPM4'),
        '-I', str(ROOT / 'kits/ArmSmmu4K'),
        '-I', str(ROOT / 'kits/AdrenoFirmware'),
    ]

    cpp_sources = sorted([
        p for p in (ROOT / 'drivers/NativeAdreno').glob('*.cpp')
        if p.name != 'NativeAdrenoAccelerator.cpp'
    ])
    c_sources = [
        ROOT / 'drivers/NativeAdreno/NativeAdrenoStubs.c',
        ROOT / 'drivers/NativeAdreno/NativeAdrenoGmuStub.c',
        ROOT / 'kits/AdrenoPM4/AdrenoPM4.c',
        ROOT / 'kits/ArmSmmu4K/ArmSmmu4KContext.c',
        ROOT / 'kits/ArmSmmu4K/ArmSmmu4KProgram.c',
        ROOT / 'kits/ArmSmmu4K/ArmSmmu4KLive.c',
        ROOT / 'kits/ArmSmmu4K/ArmSmmu4KStreams.c',
        ROOT / 'kits/ArmSmmu4K/ArmSmmu4KMmio.c',
        ROOT / 'kits/AdrenoFirmware/AdrenoHfi.c',
        ROOT / 'kits/AdrenoFirmware/AdrenoHfiStartup.c',
        ROOT / 'kits/AdrenoFirmware/AdrenoHfiVotes.c',
        ROOT / 'kits/AdrenoFirmware/AdrenoFirmware.c',
    ]

    objects = []
    for src in cpp_sources:
        dst = out / f'{src.stem}.o'
        compile_file(src, dst, record, extra_flags, is_c=False)
        objects.append(dst)

    for src in c_sources:
        if src.exists():
            dst = out / f'{src.stem}.o'
            compile_file(src, dst, record, extra_flags, is_c=True)
            objects.append(dst)

    archive = out / 'libNativeAdreno.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive)] + [str(o) for o in objects], check=True)
    print(f'NativeAdreno: ARM64/iOS 17 core driver archive built ({len(objects)} objects)')

    # Integrate staged AGXAccelerator via build-native-adreno-accelerator.py
    accel_script = ROOT / 'scripts/build-native-adreno-accelerator.py'
    if accel_script.exists():
        env = os.environ.copy()
        if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app/Contents/Developer').is_dir():
            env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
        res = subprocess.run([sys.executable, str(accel_script)], cwd=str(ROOT), env=env, capture_output=True, text=True)
        if res.returncode != 0:
            print(f'Warning: build-native-adreno-accelerator.py failed:\n{res.stderr}')
        else:
            print('NativeAdreno: integrated staged AGXAccelerator and original IOAcceleratorES imports')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=['ArmDTPlatform', 'ArmCpu', 'NativeRamBlock', 'NativeUfs', 'NativeAdreno'])
    args = parser.parse_args()

    recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))

    if args.component == 'NativeAdreno':
        build_adreno(record)
        return

    out_map = {
        'ArmCpu': ('armcpu', 'ArmCpu', 'ArmCpu.cpp', []),
        'ArmDTPlatform': ('armdt-platform', 'ArmDTPlatform', 'ArmDTPlatform.cpp', []),
        'NativeRamBlock': ('native-ram-block', 'NativeRamBlock', 'NativeRamBlock.cpp', [
            '-I', str(ROOT / 'build/storage-abi/include'),
            '-I', str(ROOT / 'drivers/NativeRamBlock'),
        ]),
        'NativeUfs': ('native-ufs', 'NativeUfs', 'NativeUfs.cpp', [
            '-I', str(ROOT / 'build/storage-abi/include'),
            '-I', str(ROOT / 'drivers/NativeUfs'),
        ]),
    }
    dir_name, sub_dir, src_file, extra_flags = out_map[args.component]
    out = ROOT / 'build' / dir_name
    source = ROOT / 'drivers' / sub_dir / src_file
    destination = out / f'{args.component}.o'

    compile_file(source, destination, record, extra_flags, is_c=False)

    extra_objects = []
    if args.component == 'NativeUfs':
        bindings_s = out / 'storage-bindings.S'
        bindings_s.write_text("""
.globl __ZN20IOBlockStorageDevice10gMetaClassE
.set __ZN20IOBlockStorageDevice10gMetaClassE, 0xfffffff007add1d0
.globl __ZN20IOBlockStorageDevice11requestIdleEv
.set __ZN20IOBlockStorageDevice11requestIdleEv, 0xfffffff00596f050
.globl __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh
.set __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh, 0xfffffff00596f068
.globl __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject
.set __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject, 0xfffffff00596ef38
.globl __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty
.set __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty, 0xfffffff00596f08c
.globl __ZN20IOBlockStorageDevice4initEP12OSDictionary
.set __ZN20IOBlockStorageDevice4initEP12OSDictionary, 0xfffffff00596ee30
.globl __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj
.set __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj, 0xfffffff00596f05c
.globl __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass
.set __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass, 0xfffffff00596ed98
.globl __ZN20IOBlockStorageDeviceD2Ev
.set __ZN20IOBlockStorageDeviceD2Ev, 0xfffffff00596edb8
.globl __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol
.set __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol, 0xfffffff00596ee88
.globl __ZTV20IOBlockStorageDevice
.set __ZTV20IOBlockStorageDevice, 0xfffffff006cb44c0
""")
        bindings_o = out / 'storage-bindings.o'
        subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(bindings_s), '-o', str(bindings_o)], check=True)
        extra_objects.append(bindings_o)

    archive = out / ('lib' + args.component + '.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(destination)] + [str(o) for o in extra_objects], check=True)
    print(f'{args.component}: ARM64/iOS 17 archive built')


if __name__ == '__main__':
    main()
