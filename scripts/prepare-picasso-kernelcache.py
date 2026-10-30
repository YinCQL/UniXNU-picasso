#!/usr/bin/env python3
"""Prepare the retained 21H461 native-kext donor independently of iOS userland.

Inputs are read-only. The output directory must be new; failed payloads remain
available for inspection. Remote reads use the existing upstream range reader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import zipfile

import picasso_target as target


def prepare(tree, ipsw, output):
    spec = target.BACKEND_IPSW
    sys.path.insert(0, str(tree.resolve() / 'scripts'))
    from unixnu.ipsw import RangeFile, im4p_payload
    remote = ipsw.startswith('https://')
    stream = RangeFile(ipsw) if remote else Path(ipsw).open('rb')
    with stream, zipfile.ZipFile(stream) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        if ((manifest['ProductVersion'], manifest['ProductBuildVersion']) != (spec['version'], spec['build']) or
                spec['product'] not in manifest['SupportedProductTypes']):
            raise ValueError('Expected the retained iPad7,1 / 21H461 kernel donor, not the iPhone userland IPSW')
        identities = [i for i in manifest['BuildIdentities'] if
                      i['Info']['DeviceClass'] == spec['device_class'] and i['Info']['RestoreBehavior'] == 'Erase' and
                      int(i['ApChipID'], 16) == spec['chip_id'] and int(i['ApBoardID'], 16) == spec['board_id']]
        if len(identities) != 1 or identities[0]['Manifest']['KernelCache']['Info']['Path'] != spec['kernel_member']:
            raise ValueError('Unexpected kernel donor identity/member')
        packed = archive.read(spec['kernel_member'])
    if hashlib.sha256(packed).hexdigest() != spec['kernel_member_sha256']:
        raise ValueError('Kernel donor member SHA256 mismatch')
    output.mkdir(parents=True, exist_ok=False)
    (output / spec['kernel_member']).write_bytes(packed)
    payload = im4p_payload(packed)
    packed_path, raw_path = output / 'kernelcache.lzfse', output / 'kernelcache.decoded'
    packed_path.write_bytes(payload)
    if sys.platform == 'darwin':
        subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse',
                        '-i', str(packed_path), '-o', str(raw_path)], check=True)
        raw = raw_path.read_bytes()
    else:
        import lzfse
        raw = lzfse.decompress(payload)
        raw_path.write_bytes(raw)
    if raw[:4] == b'\xca\xfe\xba\xbe':
        count = struct.unpack_from('>I', raw, 4)[0]
        if not 0 < count <= 16 or 8 + count * 20 > len(raw):
            raise ValueError('Invalid donor FAT header')
        slices = [struct.unpack_from('>5I', raw, 8 + i * 20) for i in range(count)]
        matches = [s for s in slices if s[0:2] == (0x100000c, 0)]
        if len(matches) != 1:
            raise ValueError('Expected one ordinary ARM64 donor slice')
        _, _, offset, size, _ = matches[0]
        if offset < 8 + count * 20 or size < 32 or offset + size > len(raw):
            raise ValueError('Donor FAT slice is out of bounds')
        raw = raw[offset:offset + size]
    if hashlib.sha256(raw).hexdigest() != target.BACKEND_KERNEL_SHA256:
        raise ValueError('Decoded kernel does not match the qualified 21H461 backend')
    if struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 2):
        raise ValueError('Expected ordinary ARM64 MH_EXECUTE donor')
    (output / 'kernelcache.decompressed').write_bytes(raw)
    report = dict(backend_build=spec['build'], member=spec['kernel_member'],
                  member_sha256=spec['kernel_member_sha256'], kernelcache_sha256=target.BACKEND_KERNEL_SHA256,
                  userland_build=target.BUILD, separate_inputs=True, bytes=len(raw),
                  source='remote' if remote else 'local', inputs_modified=False)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--ipsw', default=target.BACKEND_IPSW['url'])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.tree, args.ipsw, args.output), indent=2))


if __name__ == '__main__':
    main()
