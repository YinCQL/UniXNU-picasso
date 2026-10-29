#!/usr/bin/env python3
"""Verify actual software AES objects, aliases and linked dispatch before delivery."""
import argparse
import json
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('aes', ROOT / 'scripts/build-picasso-aes.py')
    aes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(aes)
    platform = aes.load(ROOT / 'scripts/build-picasso-pmgr.py')
    native = aes.load(args.tree / 'kits/NativePrelink/native_prelink.py')
    original = args.kernelcache.read_bytes()
    aliases, slots, _ = aes.qualify_original(native, original,
        (args.tree / 'scripts/data/native-aes-original-bindings.S').read_bytes(), aes.SLIDE)
    out = args.tree / 'build/native-aes'
    audit = aes.audit_object(platform, native, out / 'NativeAES.o', original, aliases, slots)
    aes.require(audit == json.loads((out / 'object-audit.json').read_text(encoding='utf-8')), 'Stale AES object report')
    build = json.loads((out / 'build-report.json').read_text(encoding='utf-8'))
    for path, key in [(out / 'libNativeAES.a', 'archive_sha256'),
                      (out / 'original-bindings.o', 'bindings_sha256'),
                      (out / 'NativeAES.o', 'object_sha256'),
                      (args.tree / 'drivers/NativeAES/NativeAES.cpp', 'source_sha256'),
                      (args.tree / 'drivers/NativeAES/OriginalAESABI.h', 'header_sha256')]:
        aes.require(aes.sha(path.read_bytes()) == build[key], 'Stale AES input: ' + str(path))
    kernel = args.work / 'kernel/kernel.macho'
    plan = json.loads(kernel.with_suffix('.json').read_text(encoding='utf-8'))
    report = aes.verify_kernel(native, original, kernel.read_bytes(), plan, audit, aliases, slots)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: actual software AES source/object/archive, original superclass and all linked dispatch slots')


if __name__ == '__main__':
    main()
