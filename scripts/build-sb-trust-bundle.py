#!/usr/bin/env python3
"""Extend the boot trust bundle with the SpringBoard inject library's CDHash.

  build-sb-trust-bundle.py OWNER.bundle LIB.dylib OUTPUT.bundle

OWNER is the bundle scripts/make-owner-trust-bundle.py produced (one owner
module with the Adreno plugin hashes, then the two original Apple modules byte
for byte). The output replaces the owner module's hash list with those hashes
plus the dylib's sha256 CDHash, kept in strictly ascending order as the kernel's
binary search requires, and copies both original modules unchanged.
"""
import hashlib
import json
import struct
import subprocess
import sys
import uuid
from pathlib import Path


def module_hashes(module):
    count = struct.unpack_from('<I', module, 20)[0]
    return [module[24 + index * 24:24 + index * 24 + 20] for index in range(count)]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    owner, library, output = (Path(argument) for argument in sys.argv[1:])
    bundle = owner.read_bytes()
    count = struct.unpack_from('<I', bundle)[0]
    starts = list(struct.unpack_from(f'<{count}I', bundle, 4))
    ends = starts[1:] + [len(bundle)]
    modules = [bundle[start:end] for start, end in zip(starts, ends)]

    identity = subprocess.check_output(
        ['codesign', '-dvvv', str(library)], stderr=subprocess.STDOUT).decode()
    cdhash = None
    for line in identity.splitlines():
        if line.startswith('CDHash='):
            cdhash = bytes.fromhex(line.split('=', 1)[1].strip())
    if not cdhash or len(cdhash) != 20:
        sys.exit('could not read a 20-byte CDHash from ' + str(library))

    hashes = sorted(set(module_hashes(modules[0])) | {cdhash})
    if len(hashes) != len(module_hashes(modules[0])) + 1:
        sys.exit('the library CDHash is already in the bundle')
    identifier = uuid.uuid5(uuid.NAMESPACE_URL,
                            'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in hashes))
    owner_module = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(
        h + bytes([2, 0, 0, 0]) for h in hashes)
    modules[0] = owner_module

    cursor, new_starts = 16, []
    for module in modules:
        new_starts.append(cursor)
        cursor += len(module)
    result = struct.pack(f'<{len(modules) + 1}I', len(modules), *new_starts) + b''.join(modules)
    output.write_bytes(result)
    output.with_suffix('.json').write_text(json.dumps({
        'bundle_sha256': hashlib.sha256(result).hexdigest(),
        'own_cdhashes': [h.hex() for h in hashes],
        'original_modules_unchanged': True,
        'purpose': 'owner trust for the Adreno Metal plugin and the SpringBoard inject library',
    }, indent=2) + '\n')
    print(f'picasso: wrote {output} ({len(result)} bytes), dylib CDHash {cdhash.hex()}')


if __name__ == '__main__':
    main()
