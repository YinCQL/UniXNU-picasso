#!/usr/bin/env python3
"""Verify the actual SDK display objects, aliases and linked dispatch table."""
import argparse
import json
from pathlib import Path
import subprocess
import tomllib

from importlib.util import module_from_spec, spec_from_file_location


def load(path):
    spec = spec_from_file_location(path.stem.replace('-', '_'), path)
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(tree, work, kernelcache, output):
    builder = load(tree / 'scripts/build-picasso-display.py')
    platform = load(tree / 'scripts/build-picasso-pmgr.py')
    aes = load(tree / 'scripts/build-picasso-aes.py')
    native = load(tree / 'kits/NativePrelink/native_prelink.py')
    directory = tree / 'build/native-legacy-display'
    build = json.loads((directory / 'build-report.json').read_text())
    board = tomllib.loads((Path(__file__).resolve().parents[1] / 'board.toml').read_text(encoding='utf-8'))
    profile = builder.build_profile(board)
    builder.require(build.get('build_profile') == profile, 'Display build profile differs from the selected inputs')
    builder.require(build['complete'], 'Incomplete display build')
    for name, digest in build['source_sha256'].items():
        builder.require(builder.sha((tree / name).read_bytes()) == digest, 'Stale display source ' + name)
    original = kernelcache.read_bytes()
    slots, aliases, symbols = builder.qualify(native, original)
    audit = builder.audit_object(platform, native, directory / 'NativeLegacyDisplay.o', slots, aliases, symbols)
    recorded = json.loads((directory / 'object-audit.json').read_text())
    builder.require(audit == recorded and builder.sha((directory / 'object-audit.json').read_bytes()) == build['object_audit_sha256'],
                    'Stale display object audit')
    expected_header = builder.header((tree / 'drivers/NativeMobileDisplay/OriginalLegacyFramebufferBase.h.in').read_text())
    builder.require((directory / 'OriginalLegacyFramebufferABI.h').read_text() == expected_header and
                    builder.sha((directory / 'OriginalLegacyFramebufferABI.h').read_bytes()) == build['generated_abi_sha256'],
                    'Changed display ABI header')
    initializer = '__GLOBAL__sub_I_NativeLegacyDisplay.cpp'
    builder.require(platform.initializer_state(directory / 'NativeLegacyDisplay.o', native, initializer, True) ==
                    platform.initializer_state(directory / 'NativeLegacyDisplay.late.o', native, initializer, False),
                    'Late conversion changed display instructions')
    initializer = '__GLOBAL__sub_I_NativeKeyStoreEvents.cpp'
    builder.require(platform.initializer_state(directory / 'NativeKeyStoreEvents.o', native, initializer, True) ==
                    platform.initializer_state(directory / 'NativeKeyStoreEvents.late.o', native, initializer, False),
                    'Late conversion changed state-service instructions')
    archive = directory / 'libNativeLegacyDisplay.late.a'
    builder.require(builder.sha(archive.read_bytes()) == build['archive_sha256'], 'Stale display archive')
    for name in build['members']:
        member = subprocess.check_output(['llvm-ar', 'p', str(archive), name])
        builder.require(member == (directory / name).read_bytes(), 'Stale display archive member ' + name)
    builder.require(set(build['members']) == {'NativeLegacyDisplay.late.o', 'PicassoDisplay.o',
        'NativeKeyStoreEvents.late.o', 'PicassoKeyStore.o', 'original-bindings.o'}, 'Wrong display/state archive members')
    kernel = work / 'kernel/kernel.macho'
    plan = json.loads(kernel.with_suffix('.json').read_text())
    report = builder.verify_kernel(aes, native, original, kernel.read_bytes(), plan, audit)
    config = (work / '.config').read_text()
    builder.require(config.splitlines().count('CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y') == 1, 'Display disabled in actual config')
    builder.require('CONFIG_XNU_NATIVE_ADRENO=y' not in config, 'Unexpected GPU configuration')
    report.update(scope='Actual SDK build and final linked dispatch; runtime not qualified',
                  archive_sha256=build['archive_sha256'], build_profile=profile)
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    check(args.tree.resolve(), args.work.resolve(), args.kernelcache.resolve(), args.output.resolve())
