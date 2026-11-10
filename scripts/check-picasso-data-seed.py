#!/usr/bin/env python3
"""Exercise seed parameter, APFS identity and input preservation boundaries."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
import uuid

import picasso_data_seed as seed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    cases = []

    def reject(name, function):
        try:
            function()
        except ValueError:
            cases.append(name)
        else:
            raise AssertionError('Accepted ' + name)

    original = '11111111-2222-4333-8444-555555555555'
    requested = '66666666-7777-4888-8999-AAAAAAAAAAAA'
    for size in ('4', '20'):
        for skip in ('true', 'false'):
            for identity in ('', requested.lower()):
                value = seed.options(size, identity, skip, '90')
                assert value['data_uuid'] == (requested if identity else '')
                cases.append(f'parameters-{size}-{skip}-{bool(identity)}')
    for size in ('0', '3', '21', '4.5', '04', '-4', '20\n', '$(id)'):
        reject('bad-size-' + repr(size), lambda: seed.options(size, '', 'true', '90'))
    for identity in ('0' * 32, '00000000-0000-0000-0000-000000000000', requested + '\n', '$(id)', '../seed'):
        reject('bad-uuid-' + repr(identity), lambda: seed.options('20', identity, 'true', '90'))
    for days in ('0', '91', '1.5', '-1', '7\n'):
        reject('bad-retention-' + repr(days), lambda: seed.options('20', '', 'true', days))
    reject('bad-boolean', lambda: seed.options('20', '', 'yes', '90'))
    with tempfile.TemporaryDirectory(prefix='picasso-seed-host-') as temporary:
        work = Path(temporary)
        packed, raw = work / 'fixture.dbrdisk', work / 'fixture.cdr'
        block = bytearray(seed.PAGE)
        struct.pack_into('<I', block, 24, 13)
        block[32:36] = b'APSB'
        block[240:256] = uuid.UUID(original).bytes
        struct.pack_into('<Q', block, 264, 1)
        block[:8] = seed.checksum(block)
        # Verify the checksum with the independent zero-residue formulation.
        first = second = 0
        for (word,) in struct.iter_unpack('<I', block[8:] + block[:8]):
            first = (first + word) % 0xffffffff
            second = (second + first) % 0xffffffff
        assert first == second == 0

        def save(blocks):
            blob = struct.pack('<8sIIQQ', b'DBRDISK1', 1, seed.PAGE, 4 * seed.GIB, len(blocks))
            blob += b''.join(struct.pack('<QQ', (i + 1) * seed.PAGE, seed.PAGE) for i in range(len(blocks)))
            packed.write_bytes(blob + b''.join(blocks))

        save([block, block])
        assert seed.packed_uuid(packed, args.tree) == original
        seed.expand(packed, raw, args.tree)
        report = seed.assign_uuid(raw, packed, args.tree, original, requested)
        assert report['checkpoints'] == 2 and seed.packed_uuid(packed, args.tree) == original
        with raw.open('rb') as stream:
            assert stream.read(seed.PAGE) == bytes(seed.PAGE)
            for _ in range(2):
                after = stream.read(seed.PAGE)
                assert after[240:256] == uuid.UUID(requested).bytes and after[:8] == seed.checksum(after)
                assert after[8:240] == block[8:240] and after[256:] == block[256:]
        cases.append('all-checkpoints-only-uuid-and-checksum-changed-base-preserved')
        reject('raw-checkpoint-mismatch', lambda: seed.assign_uuid(raw, packed, args.tree, original, requested))
        bad = bytearray(block); bad[300] ^= 1
        save([bad]); reject('corrupt-checksum', lambda: seed.volume_blocks(packed, args.tree))
        for name, offset, value in (('encrypted-volume', 264, 0), ('snapshots', 216, 1)):
            bad = bytearray(block); struct.pack_into('<Q', bad, offset, value); bad[:8] = seed.checksum(bad)
            save([bad]); reject(name, lambda: seed.volume_blocks(packed, args.tree))
        bad = bytearray(block); bad[240:256] = uuid.UUID(requested).bytes; bad[:8] = seed.checksum(bad)
        save([block, bad]); reject('multiple-volumes', lambda: seed.volume_blocks(packed, args.tree))
        unchanged = {'com.apple.migration': None, 'com.apple.purplebuddy': None}
        settings = dict(unchanged, **{'com.apple.springboard': {'BSUIVibrancyEffectViewDisabled': True},
                        'com.apple.UIKit': {'UIStatusBarVisualProviderClassName': '_UIStatusBarVisualProvider_Pad_ForcedNoCell',
                                           'StatusBarHidesDate': True}})
        seed.verify_settings(settings, False, unchanged)
        cases.append('setup-enabled-preserves-original-preferences')
        settings['com.apple.purplebuddy'] = {'SetupDone': True}
        reject('setup-enabled-was-marked-done', lambda: seed.verify_settings(settings, False, unchanged))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    seed.write_json(args.output, dict(passed=True, cases=cases, scope='host fixtures; no APFS mount or kernel qualification'))
    print(f'PASS: {len(cases)} seed generation boundaries')


if __name__ == '__main__':
    main()
