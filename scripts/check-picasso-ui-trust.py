#!/usr/bin/env python3
"""Check code-slot hashes and the exact one-library trust boundary on the host."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import picasso_ui_trust as trust


def fixture_library(page_shift=12):
    code = bytearray(1 << page_shift)
    struct.pack_into('<8I', code, 0, 0xfeedfacf, 0x100000c, 0, 6, 1, 16, 0, 0)
    identifier = b'libpicasso-sb\0'
    directory = bytearray(44 + len(identifier) + 32)
    struct.pack_into('>9I', directory, 0, 0xfade0c02, len(directory), 0x20400, 2,
                     44 + len(identifier), 44, 0, 1, len(code))
    directory[36:40] = bytes([32, 2, 0, page_shift])
    directory[44:44 + len(identifier)] = identifier
    length = 20 + len(directory)
    struct.pack_into('<4I', code, 32, 0x1d, 16, len(code), length)
    directory[-32:] = hashlib.sha256(code).digest()
    signature = struct.pack('>5I', 0xfade0cc0, length, 1, 0, 20) + directory
    return bytes(code) + signature


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--original', type=Path)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--signed-library', type=Path, help='Validate the host codesign output with structural trust fixtures')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    library = fixture_library()
    # A small structural fixture covers rejection paths; it is not Apple data.
    modules = [struct.pack('<I16sI', 2, bytes([i]) * 16, 1) + bytes([i]) * 20 + bytes([2, 0, 0, 0]) for i in (1, 2)]
    original = struct.pack('<3I', 2, 12, 12 + len(modules[0])) + b''.join(modules)
    pinned = trust.TRUST_BUNDLE_SHA256
    trust.TRUST_BUNDLE_SHA256 = hashlib.sha256(original).hexdigest()
    count = 0
    def rejected(fn):
        nonlocal count
        try:
            fn()
        except (ValueError, OSError):
            count += 1
            return
        raise AssertionError('Invalid trust input accepted')
    bundle, report = trust.make_bundle(original, library)
    assert trust.verify_bundle(bundle, library) == report
    assert report['owned_entries'] == 1 and report['original_modules_unchanged']
    count += 1
    for offset in (0, 4, 12, 20, 36, 4096, 4104, 4112, len(library) - 1):
        bad = bytearray(library); bad[offset] ^= 1
        rejected(lambda bad=bad: trust.verify_bundle(bundle, bytes(bad)))
    for offset in (0, 4, 8, 12, 16, 40, len(bundle) - 24, len(bundle) - 4, len(bundle) - 1):
        bad = bytearray(bundle); bad[offset] ^= 1
        rejected(lambda bad=bad: trust.verify_bundle(bytes(bad), library))
    for length in (0, 8, 31, 40, 4096, len(library) - 1):
        rejected(lambda length=length: trust.library_identity(library[:length]))
    for length in (0, 15, len(bundle) - 1):
        rejected(lambda length=length: trust.verify_bundle(bundle[:length], library))
    rejected(lambda: trust.verify_bundle(bundle + bytes(24), library))
    rejected(lambda: trust.accepted_payload_bundle(bundle, '../libpicasso-sb.dylib', args.output))
    large_page = fixture_library(14)
    large_bundle, large_report = trust.make_bundle(original, large_page)
    assert large_report['library']['code_page_bytes'] == 16384
    assert trust.verify_bundle(large_bundle, large_page) == large_report
    count += 1
    for offset in (4096, 8192, 16384 + 20 + 39):
        bad = bytearray(large_page); bad[offset] ^= 1
        rejected(lambda bad=bad: trust.verify_bundle(large_bundle, bytes(bad)))
    signed = None
    if args.signed_library:
        data = args.signed_library.read_bytes()
        signed_bundle, signed = trust.make_bundle(original, data)
        assert trust.verify_bundle(signed_bundle, data) == signed
        count += 1
        for offset in (64, 4096, 16384, 32768):
            if offset >= len(data):
                continue
            bad = bytearray(data); bad[offset] ^= 1
            rejected(lambda bad=bad: trust.verify_bundle(signed_bundle, bytes(bad)))
    trust.TRUST_BUNDLE_SHA256 = pinned
    actual = None
    if args.original or args.library:
        if not args.original or not args.library:
            raise ValueError('Supply both original bundle and signed library')
        actual_bundle, actual = trust.make_bundle(args.original.read_bytes(), args.library.read_bytes())
        assert trust.verify_bundle(actual_bundle, args.library.read_bytes()) == actual
        (args.output / 'actual.bundle').write_bytes(actual_bundle)
        count += 1
    (args.output / 'results.json').write_text(json.dumps(dict(pass_=True, checks=count,
        scope=__doc__, actual=actual, signed_host_library=signed), indent=2) + '\n', encoding='utf-8')
    print('PASS UI trust:', count, 'checks; signed-library and original-module mutations rejected')


if __name__ == '__main__':
    main()
