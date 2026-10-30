#!/usr/bin/env python3
"""Check iPhone X input configuration and report unfinished kernel migration.

--require-build-ready deliberately fails until the Darwin 22 backend and signed
root policy have been ported. Input/ADT checks alone do not qualify a boot image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import sys
import tomllib
import uuid
import zipfile

import picasso_target as target

ROOT = Path(__file__).resolve().parents[1]


def check_board(root=ROOT):
    board = tomllib.loads((root / 'board.toml').read_text(encoding='utf-8'))
    spec = board['ipsw']
    expected = dict(product=target.PRODUCT, device_class=target.DEVICE_CLASS,
                    version=target.VERSION, build=target.BUILD, chip_id=target.CHIP_ID,
                    board_id=target.BOARD_ID, url=target.URL)
    if any(spec.get(k) != v for k, v in expected.items()):
        raise ValueError('board.toml does not select the audited d22ap Erase identity')
    if spec['sha256'] != {Path(p).name: digest for p, digest in target.COMPONENTS.values()}:
        raise ValueError('IPSW member pins differ from the audited inputs')
    if board['kernel']['prelink']['kernelcache_sha256'] != target.KERNEL_SHA256:
        raise ValueError('Board kernelcache pin does not match iPhone X')
    if 'early_security_init' in board['kernel']['prelink']:
        raise ValueError('iPhone X AMFI initializer is not qualified; do not reuse the iPad address')
    adt = board['payload']['adt']
    if adt.get('iphone_identity') is not True or adt.get('ipad_identity'):
        raise ValueError('Require only the iPhone identity in the payload')
    if adt['root_match_volume_uuid'] != target.OS_UUID:
        raise ValueError('ADT root UUID does not match iPhone OS')
    if board['payload']['trust_cache_sha256'] != [target.TRUST_BUNDLE_SHA256]:
        raise ValueError('Board trust bundle pin does not match the original iPhone caches')
    if board['userdata']['windows'] != [{'name': 'OS', 'component': 'OS'},
                                      {'name': 'SystemOS', 'component': 'Cryptex1,SystemOS'}]:
        raise ValueError('The iPhone firmware still requires both original volumes')
    config = (root / 'configs/picasso_defconfig').read_text()
    if config.count(f'CONFIG_XNU_OWNER_CACHE_VOLUME_UUID="{target.SYSTEMOS_UUID}"') != 1:
        raise ValueError('Configured SystemOS UUID differs from the target volume')
    check_policy(root / 'policy/verified-root-picasso')
    return board


def check_policy(path):
    """Pin the exact host-verified signed artifacts, without claiming device validation."""
    audit = json.loads((path / 'attestation.json').read_text(encoding='utf-8'))
    anchor = (path / 'trusted-key.bin').read_bytes()
    if hashlib.sha256(anchor).hexdigest() != target.OWNER_ANCHOR_SHA256:
        raise ValueError('Root policy changed the existing owner anchor')
    if (audit['root_image']['sha256'] != target.ROOT_IMAGE_SHA256 or
            audit['root_image']['bytes'] != target.ROOT_IMAGE_BYTES):
        raise ValueError('Root policy still authenticates a different userdata image')
    if hashlib.sha256((path / 'policy.S').read_bytes()).hexdigest() != audit['policy_assembly_sha256']:
        raise ValueError('Root policy assembly differs from the signed artifact report')
    if [r['name'] for r in audit['windows']] != ['OS', 'SystemOS']:
        raise ValueError('Root policy must contain OS then SystemOS')
    for expected, record in zip(target.ROOT_WINDOWS, audit['windows']):
        directory = path / expected['directory']
        manifest = (directory / 'manifest.bin').read_bytes()
        if hashlib.sha256(manifest).hexdigest() != target.POLICY_MANIFEST_SHA256[expected['name']]:
            raise ValueError('Unaudited signed policy: ' + expected['name'])
        header, signature, table = manifest[:160], manifest[160:544], manifest[544:]
        count = (expected['size'] + 131071) // 131072
        if (struct.unpack_from('<8s4I2Q', header) != (b'VROOT01\0', 1, 160, 131072, 0, expected['size'], count) or
                header[40:56] != uuid.UUID(expected['volume_uuid']).bytes or
                header[56:88].hex() != expected['sha256'] or
                header[88:120] != hashlib.sha256(table).digest() or
                header[120:152] != hashlib.sha256(anchor).digest() or
                struct.unpack_from('<Q', header, 152)[0] != 1 or len(table) != count * 32):
            raise ValueError('Invalid signed policy identity or table')
        for name, contents in [('header.bin', header), ('signature.bin', signature),
                               ('header-signature.bin', header + signature),
                               ('hashes.bin', table), ('trusted-key.bin', anchor)]:
            if (directory / name).read_bytes() != contents:
                raise ValueError('Policy split file differs from its pinned manifest: ' + name)
        if any(record[k] != expected[k] for k in ('name', 'offset', 'size', 'volume_uuid')) or record['image_sha256'] != expected['sha256']:
            raise ValueError('Policy attestation window differs from the audited target')


def check_ipsw(path):
    with path.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != target.IPSW_SHA256:
            raise ValueError('Whole IPSW SHA256 differs from the audited file')
    with zipfile.ZipFile(path) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != (target.VERSION, target.BUILD):
            raise ValueError('IPSW version/build mismatch')
        if target.PRODUCT not in manifest['SupportedProductTypes']:
            raise ValueError('IPSW does not support iPhone10,3')
        identities = [v for v in manifest['BuildIdentities'] if
                      v['Info']['DeviceClass'] == target.DEVICE_CLASS and
                      v['Info']['RestoreBehavior'] == 'Erase' and
                      int(v['ApChipID'], 16) == target.CHIP_ID and
                      int(v['ApBoardID'], 16) == target.BOARD_ID]
        if len(identities) != 1:
            raise ValueError('Require one d22ap Erase identity')
        for component, (member, _) in target.COMPONENTS.items():
            if identities[0]['Manifest'][component]['Info']['Path'] != member:
                raise ValueError('Unexpected IPSW component path: ' + component)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipsw', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--github-env', type=Path)
    parser.add_argument('--require-build-ready', action='store_true')
    args = parser.parse_args()
    check_board()
    if args.ipsw:
        check_ipsw(args.ipsw)
    report = dict(input_configuration_valid=True, product=target.PRODUCT, version=target.VERSION,
                  build=target.BUILD, kernel_darwin=22, build_ready=False,
                  blockers=target.BUILD_BLOCKERS, ipsw_checked=args.ipsw is not None)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2); stream.write('\n')
    if args.require_build_ready:
        target.require_build_ready()
    if args.github_env:
        values = dict(PICASSO_IPSW_FILE=target.FILENAME, PICASSO_IPSW_URL=target.URL,
                      PICASSO_IPSW_BUILD=target.BUILD, PICASSO_OS_MEMBER=target.OS_MEMBER)
        with args.github_env.open('a', encoding='utf-8') as stream:
            for name, value in values.items():
                stream.write(f'{name}={value}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
