#!/usr/bin/env python3
"""Bundle the two pinned, unchanged iPhone X 20H392 trust-cache modules.

The original Apple static and System trust caches are the only trust modules
this board feeds to the kernel: DarwinBoot copies the bundle into the kernel
window and writes its address and size into the ADT TrustCache placeholder,
which is what makes the kernel load the external caches at all. Both modules
are read from the pinned IPSW, verified byte for byte, and packaged as the
count/offsets/modules layout DarwinBoot ValidTrustCaches accepts.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
from picasso_target import TRUST_MODULES, TRUST_BUNDLE_SHA256


def bundle_modules(modules):
    expected = tuple(digest for _, digest in TRUST_MODULES)
    if len(modules) != 2:
        raise ValueError('Require static and System trust-cache modules')
    for module, digest in zip(modules, expected):
        if (hashlib.sha256(module).hexdigest() != digest or len(module) < 24 or
                struct.unpack_from('<I', module)[0] != 2 or
                len(module) != 24 + struct.unpack_from('<I', module, 20)[0] * 24):
            raise ValueError('Require complete, exact original version-2 trust modules')
    bundle = struct.pack('<3I', 2, 12, 12 + len(modules[0])) + b''.join(modules)
    if hashlib.sha256(bundle).hexdigest() != TRUST_BUNDLE_SHA256:
        raise ValueError('Original trust bundle differs')
    return bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.tree / 'scripts'))
    from unixnu.ipsw import im4p_payload
    modules = [im4p_payload((args.inputs / name).read_bytes()) for name, _ in TRUST_MODULES]
    bundle = bundle_modules(modules)
    # Exercise fail-closed handling against truncation, reordering and mutation.
    for bad in ([modules[0][:-1], modules[1]], list(reversed(modules)),
                [bytes([modules[0][0] ^ 1]) + modules[0][1:], modules[1]]):
        try:
            bundle_modules(bad)
        except ValueError:
            continue
        raise AssertionError('Modified trust module accepted')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    print(f'PASS: two unchanged original trust modules, {len(bundle)} bytes; negative checks passed')


if __name__ == '__main__':
    main()
