#!/usr/bin/env python3
"""Host regression for seed format, copy/metadata checks and UUID configuration.

All generated inputs remain in --output. Windows checks filesystem contents and
uses mocked chown calls; only macOS/root preparation validates APFS and ownership.
"""
import argparse
from contextlib import nullcontext
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import runpy
import stat
import struct
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_prepare():
    spec = importlib.util.spec_from_file_location('picasso_data', ROOT / 'scripts/prepare-picasso-ram-data.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--packer', type=Path, help='Optional fixed DarwinBoot Test/packed_ramdisk.py')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    data = load_prepare()
    cases = []

    def passes(name, operation):
        operation()
        cases.append({'case': name, 'result': 'pass'})

    def rejected(name, operation):
        try:
            operation()
        except (ValueError, FileExistsError):
            cases.append({'case': name, 'result': 'rejected as expected'})
        else:
            raise AssertionError(f'{name}: invalid input was accepted')

    # Independent fixture encoding, not the production parser/packer.
    page = 4096
    raw_bytes = b'A' * page + b'B' * page + bytes(page * 2) + b'C' * page + bytes(page)
    raw = out / 'sample.cdr'
    raw.write_bytes(raw_bytes)
    packed_bytes = (struct.pack('<8sIIQQ', b'DBRDISK1', 1, page, len(raw_bytes), 2)
                    + struct.pack('<QQQQ', 0, 2 * page, 4 * page, page)
                    + raw_bytes[:2 * page] + raw_bytes[4 * page:5 * page])
    packed = out / 'sample.dbrdisk'
    packed.write_bytes(packed_bytes)

    def verify_sample():
        result = data.inspect_packed(packed, raw)
        assert result == dict(logical_bytes=len(raw_bytes), packed_bytes=len(packed_bytes),
                              raw_sha256=hashlib.sha256(raw_bytes).hexdigest(),
                              packed_sha256=hashlib.sha256(packed_bytes).hexdigest(),
                              page_size=page, extent_count=2)

    passes('reconstruct data extents and zero holes exactly', verify_sample)
    if args.packer:
        fixed_pack = runpy.run_path(str(args.packer.resolve()))['pack']

        def verify_fixed():
            target = out / 'fixed-packer.dbrdisk'
            fixed_pack(raw, target)
            assert target.read_bytes() == packed_bytes
            assert data.inspect_packed(target, raw) == data.inspect_packed(packed, raw)

        passes('actual fixed DarwinBoot packer matches independent bytes', verify_fixed)

    def mutate(name, offset, fmt, *values):
        blob = bytearray(packed_bytes)
        struct.pack_into(fmt, blob, offset, *values)
        path = out / (name + '.dbrdisk')
        path.write_bytes(blob)
        rejected(name, lambda: data.inspect_packed(path))

    mutate('wrong magic', 0, '<8s', b'NOTDISK1')
    mutate('wrong format version', 8, '<I', 2)
    mutate('wrong page size', 12, '<I', 16384)
    mutate('zero logical size', 16, '<Q', 0)
    mutate('unaligned logical size', 16, '<Q', len(raw_bytes) + 1)
    mutate('persistent-only logical size', 16, '<Q', 2 * 1024**3 + page)
    mutate('extent count overflow', 24, '<Q', 65537)
    mutate('extent table truncated', 24, '<Q', 4096)
    mutate('zero extent length', 40, '<Q', 0)
    mutate('unaligned extent start', 32, '<Q', 1)
    mutate('unaligned extent length', 40, '<Q', page + 1)
    mutate('overlapping extents', 48, '<Q', page)
    mutate('extent outside logical disk', 48, '<Q', len(raw_bytes))
    for name, blob in (('short header', packed_bytes[:31]),
                       ('truncated content', packed_bytes[:-1]),
                       ('trailing content', packed_bytes + b'x')):
        path = out / (name + '.dbrdisk')
        path.write_bytes(blob)
        rejected(name, lambda path=path: data.inspect_packed(path))
    oversized = out / 'oversized.dbrdisk'
    with oversized.open('xb') as stream:
        stream.truncate(4 * 1024**2 + 1)
    rejected('packed seed exceeds 4 MiB', lambda: data.inspect_packed(oversized))
    altered_raw = out / 'altered.cdr'
    altered_raw.write_bytes(raw_bytes[:2 * page] + b'X' + raw_bytes[2 * page + 1:])
    rejected('nonzero byte hidden in packed zero hole', lambda: data.inspect_packed(packed, altered_raw))
    short_raw = out / 'short.cdr'
    short_raw.write_bytes(raw_bytes[:-1])
    rejected('raw size mismatch', lambda: data.inspect_packed(packed, short_raw))
    empty_data = out / 'empty256.dbrdisk'
    empty_data.write_bytes(struct.pack('<8sIIQQ', b'DBRDISK1', 1, page, data.DATA_BYTES, 0))

    def verify_sparse():
        result = data.inspect_packed(empty_data)
        digest = hashlib.sha256()
        for _ in range(256):
            digest.update(bytes(1024**2))
        assert result['logical_bytes'] == data.DATA_BYTES
        assert result['raw_sha256'] == digest.hexdigest()

    passes('256 MiB logical empty disk hashes without dense allocation', verify_sparse)

    identity = '10203040-5060-7080-9010-203040506070'
    config = ('# preserve this comment\nCONFIG_XNU_NATIVE_RAM_BLOCK=y\n'
              'CONFIG_XNU_OWNER_DATA_MOUNT=y\nCONFIG_XNU_OWNER_BOOT_MANIFEST=y\n'
              'CONFIG_XNU_OWNER_DATA_VOLUME_UUID=""\nCONFIG_UNRELATED="literal"\n')

    def verify_config():
        updated = data.configured_text(config, identity.lower())
        assert updated == config.replace('CONFIG_XNU_OWNER_DATA_VOLUME_UUID=""',
                                         f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{identity}"')

    passes('actual UUID changes only the intended config setting', verify_config)
    rejected('zero UUID', lambda: data.configured_text(config, str(__import__('uuid').UUID(int=0))))
    rejected('malformed UUID', lambda: data.configured_text(config, 'Data'))
    rejected('duplicate UUID settings', lambda: data.configured_text(
        config + 'CONFIG_XNU_OWNER_DATA_VOLUME_UUID=""\n', identity))
    rejected('missing Data selector', lambda: data.configured_text(
        config.replace('CONFIG_XNU_OWNER_DATA_MOUNT=y\n', ''), identity))

    source, target = out / 'original-var', out / 'copied-var'
    source.mkdir()
    target.mkdir()
    (source / 'db').mkdir()
    (source / 'db/file').write_bytes(b'original bytes\x00with binary tail')
    (source / 'staged_system_apps').mkdir()
    (source / 'staged_system_apps/excluded').write_bytes(b'not in Data')
    link_ok = True
    try:
        os.symlink('/usr/share/zoneinfo.default', source / 'db/zoneinfo')
    except OSError as error:
        link_ok = False
        cases.append({'case': 'native symlink creation', 'result': 'unavailable on host', 'error': str(error)})
    manifest = data.inventory(source)
    assert not any('staged_system_apps' in key for key in manifest)
    # Real content copies and readback run on every host. Windows lacks chown;
    # record the required calls, while POSIX hosts execute their real chown.
    chown_context = mock.patch.object(data.os, 'chown', create=True) if os.name == 'nt' else nullcontext()
    with chown_context as chown:
        passes('copy real bytes and symlinks; preserve inventoried mode',
               lambda: data.copy_var(source, target, manifest))
        if os.name == 'nt':
            assert chown.call_count == len(manifest)
            if link_ok:
                link_calls = [call for call in chown.call_args_list if call.args[0] == target / 'db/zoneinfo']
                assert len(link_calls) == 1 and link_calls[0].kwargs == {'follow_symlinks': False}
            cases.append({'case': 'chown arguments / symlink no-follow',
                          'result': 'pass with mocked Windows ownership calls'})
    assert (target / 'db/file').read_bytes() == (source / 'db/file').read_bytes()
    if link_ok:
        assert os.readlink(target / 'db/zoneinfo') == '/usr/share/zoneinfo.default'
    # Deliberate corruption must be detected, not hidden by a correct size.
    (target / 'db/file').write_bytes(b'corrupted bytes')
    rejected('copied content changed', lambda: data.verify_var(target, manifest))
    rejected('copy refuses existing destination', lambda: data.copy_var(source, target, manifest))
    # A file used as an intermediate runtime directory must fail before following it.
    unsafe = out / 'unsafe-directories'
    unsafe.mkdir()
    (unsafe / 'db').write_bytes(b'not a directory')
    rejected('runtime directory collides with file', lambda: data.add_directories(unsafe))
    if link_ok:
        linked = out / 'symlink-directories'
        linked.mkdir()
        os.symlink(str(source / 'db'), linked / 'db', target_is_directory=True)
        rejected('runtime directory traverses a symlink', lambda: data.add_directories(linked))
        audited = out / 'mounted-source-audit'
        audited.mkdir()
        os.symlink('/Library', audited / 'Library', target_is_directory=True)
        (audited / 'usr').mkdir()
        os.symlink('../../outside', audited / 'usr/share', target_is_directory=True)

        def verify_audit():
            absolute = data.audit_path(audited, 'Library/Preferences')
            parent = data.audit_path(audited, 'usr/share/zoneinfo.default')
            assert absolute == dict(type='blocked', at='/Library',
                                    object=data.metadata(audited / 'Library'), unresolved='Preferences')
            assert parent == dict(type='blocked', at='/usr/share',
                                  object=data.metadata(audited / 'usr/share'), unresolved='zoneinfo.default')
            assert absolute['object']['target'] == '/Library'
            assert parent['object']['target'] == '../../outside'
            assert data.audit_path(audited, 'Library')['type'] == 'symlink'

        passes('source audit blocks absolute and parent-directed intermediate symlinks', verify_audit)
    rejected('source audit rejects parent traversal', lambda: data.audit_path(source, '../outside'))

    # Exercise metadata mismatch independently of the host's available UID model.
    fake = {'uid': 501, 'gid': 501, 'mode': 0o700, 'type': 'directory'}
    with mock.patch.object(data, 'metadata', return_value={**fake, 'uid': 0}):
        rejected('ownership mismatch is fatal', lambda: data.verify_var(target, {'.': fake}))
    with mock.patch.object(data, 'metadata', return_value={**fake, 'mode': 0o755}):
        rejected('permissions mismatch is fatal', lambda: data.verify_var(target, {'.': fake}))

    # Mock hdiutil/diskutil outputs to check mount selection and guaranteed detach.
    # These tests do not establish that macOS accepted or formatted an image.
    def mount_case(name, info, readonly, reject=False, body_error=False):
        mountpoint = out / name
        attached = {'system-entities': [
            {'dev-entry': '/dev/disk42'},
            {'dev-entry': '/dev/disk43s1', 'mount-point': str(mountpoint)},
        ]}
        with mock.patch.object(data.subprocess, 'check_output', side_effect=[
                plistlib.dumps(attached), plistlib.dumps(info)]) as command, \
                mock.patch.object(data.subprocess, 'run') as detach:
            def operation():
                with data.mounted(raw, mountpoint, readonly=readonly) as volume:
                    assert volume == info
                    if body_error:
                        raise ValueError('Failure while copying the seed')
            if reject or body_error:
                rejected(name, operation)
            else:
                passes(name, operation)
            detach.assert_called_once_with(['hdiutil', 'detach', '/dev/disk42'], check=True)
            attach = command.call_args_list[0].args[0]
            assert attach[attach.index('-owners') + 1] == 'on'
            assert ('-readonly' in attach) == readonly

    readonly_info = dict(FilesystemType='apfs', VolumeReadOnly=True, GlobalPermissionsEnabled=True)
    mount_case('source read-only APFS selection', readonly_info, True)
    mount_case('source rejects writable mount', {**readonly_info, 'VolumeReadOnly': False}, True, reject=True)
    mount_case('seed rejects owners-off mount', {**readonly_info, 'GlobalPermissionsEnabled': False}, True, reject=True)
    mount_case('seed failure still detaches', readonly_info, True, body_error=True)
    report = dict(scope='Host format/config/content regression; no APFS mount or device validation',
                  host=os.name, source_sha256=data.sha256(ROOT / 'scripts/prepare-picasso-ram-data.py'),
                  native_posix_ownership_tested=os.name != 'nt', cases=cases)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: {len(cases)} seed cases; outputs retained in {out}')


if __name__ == '__main__':
    main()
