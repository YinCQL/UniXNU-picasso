#!/usr/bin/env python3
"""Host regression for seed format, copy/metadata checks and UUID configuration.

All generated inputs remain in --output. Offline policy fixtures simulate root
ownership on every host so unprivileged CI can run; POSIX copy checks retain real
ownership calls. Only macOS/root preparation validates APFS policy ownership.
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
from types import SimpleNamespace
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
    empty_data = out / 'empty512.dbrdisk'
    empty_data.write_bytes(struct.pack('<8sIIQQ', b'DBRDISK1', 1, page, 512 * 1024**2, 0))

    def verify_sparse():
        result = data.inspect_packed(empty_data)
        digest = hashlib.sha256()
        for _ in range(512):
            digest.update(bytes(1024**2))
        assert result['logical_bytes'] == data.DATA_BYTES
        assert result['raw_sha256'] == digest.hexdigest()

    passes('512 MiB logical empty disk hashes without dense allocation', verify_sparse)

    payload = {'offline_disabled_jobs': ['com.apple.CommCenter'],
               'adt': {'packed_data_ramdisk': True}}
    def verify_policy_config():
        assert data.offline_jobs(payload) == ['com.apple.CommCenter']

    passes('explicit volatile telephony job policy', verify_policy_config)
    for name, jobs in [('watchdog exclusion', ['com.apple.watchdogd']),
                       ('multiple services', ['com.apple.CommCenter', 'com.apple.mediaserverd']),
                       ('duplicate service', ['com.apple.CommCenter'] * 2),
                       ('wrong policy type', 'com.apple.CommCenter')]:
        rejected(name, lambda jobs=jobs: data.offline_jobs({**payload, 'offline_disabled_jobs': jobs}))
    for adt in ({}, {'packed_data_ramdisk': False}, {'packed_data_ramdisk': True, 'persistent_data': True}):
        rejected('offline policy requires volatile packed media ' + str(adt),
                 lambda adt=adt: data.offline_jobs({**payload, 'adt': adt}))
    policy_root = out / 'policy-data'
    actual_metadata = data.metadata
    policy_owners = {}

    def policy_fixture(root):
        directory = root / data.OFFLINE_POLICY_DIR
        directory.mkdir(parents=True)
        current = root
        for part in ('', *Path(data.OFFLINE_POLICY_DIR).parts):
            if part:
                current = current / part
            os.chmod(current, 0o755)
            policy_owners[current] = (0, 0)
        return directory

    policy_dir = policy_fixture(policy_root)

    def policy_metadata(path):
        result = actual_metadata(path)
        # Only fixture ancestors start as root. Created files remain non-root
        # until the production installer calls the mocked chown below.
        result['uid'], result['gid'] = policy_owners.get(Path(path), (501, 20))
        if os.name == 'nt':
            # Windows also lacks POSIX modes. POSIX hosts read real modes;
            # real bytes, types, exclusive creation and fsync run everywhere.
            result['mode'] = 0o755 if result['type'] == 'directory' else 0o644
        return result

    def policy_chown(path, uid, gid, *, follow_symlinks):
        assert follow_symlinks is False
        policy_owners[Path(path)] = (uid, gid)

    with mock.patch.object(data, 'metadata', side_effect=policy_metadata), \
            mock.patch.object(data.os, 'chown', side_effect=policy_chown, create=True) as chown:
        saved_policy = data.install_offline_policy(policy_root, ['com.apple.CommCenter'])
        assert saved_policy['disabled_jobs'] == ['com.apple.CommCenter']
        assert saved_policy['watchdogd_disabled'] is False and len(saved_policy['files']) == 2
        assert plistlib.loads((policy_dir / 'disabled.plist').read_bytes()) == {'com.apple.CommCenter': True}
        assert (policy_dir / 'disabled.migrated').read_bytes() == b''
        assert data.verify_offline_policy(policy_root, ['com.apple.CommCenter']) == saved_policy
        assert chown.call_count == 2
        assert all(call.args[1:] == (0, 0) and call.kwargs == {'follow_symlinks': False}
                   for call in chown.call_args_list)
        cases.append({'case': 'real offline files, modern marker and simulated root ownership calls', 'result': 'pass'})
        rejected('never overwrite existing policy',
                 lambda: data.install_offline_policy(policy_root, ['com.apple.CommCenter']))
        (policy_dir / 'disabled.migrated').write_bytes(b'invalid marker')
        rejected('migration marker content mismatch',
                 lambda: data.verify_offline_policy(policy_root, ['com.apple.CommCenter']))
        (policy_dir / 'disabled.migrated').write_bytes(b'')
        for field, value in [('type', 'symlink'), ('uid', 501), ('gid', 501), ('mode', 0o666)]:
            def mismatched_file(path, field=field, value=value):
                entry = policy_metadata(path)
                return {**entry, field: value} if Path(path).name == 'disabled.plist' else entry

            with mock.patch.object(data, 'metadata', side_effect=mismatched_file):
                rejected('policy file ' + field + '=' + str(value),
                         lambda: data.verify_offline_policy(policy_root, ['com.apple.CommCenter']))
        (policy_dir / 'disabled.plist').write_bytes(plistlib.dumps({'com.apple.watchdogd': True}))
        rejected('actual policy excludes watchdog',
                 lambda: data.verify_offline_policy(policy_root, ['com.apple.CommCenter']))
    for ancestor in (policy_root, policy_root / 'db', policy_dir):
        for field, value in [('type', 'symlink'), ('type', 'file'), ('uid', 501),
                             ('gid', 20), ('mode', 0o775), ('mode', 0o757)]:
            def mismatched_ancestor(path, ancestor=ancestor, field=field, value=value):
                entry = policy_metadata(path)
                return {**entry, field: value} if Path(path) == ancestor else entry

            with mock.patch.object(data, 'metadata', side_effect=mismatched_ancestor):
                rejected('policy ancestor ' + ancestor.relative_to(out).as_posix()
                         + ' ' + field + '=' + str(value),
                         lambda: data.offline_policy_directory(policy_root))
    unowned_root = out / 'unowned-policy-data'
    unowned_dir = policy_fixture(unowned_root)
    with mock.patch.object(data, 'metadata', side_effect=policy_metadata), \
            mock.patch.object(data.os, 'chown', create=True) as chown:
        rejected('policy files must acquire root ownership before readback',
                 lambda: data.install_offline_policy(unowned_root, ['com.apple.CommCenter']))
        assert chown.call_count == 2
        assert (unowned_dir / 'disabled.plist').is_file()
        assert (unowned_dir / 'disabled.migrated').is_file()
    failed_root = out / 'failed-policy-data'
    failed_dir = policy_fixture(failed_root)
    with mock.patch.object(data, 'metadata', side_effect=policy_metadata), \
            mock.patch.object(data.os, 'chown', side_effect=policy_chown, create=True) as chown, \
            mock.patch.object(data.os, 'fsync', side_effect=OSError('simulated sync failure')):
        try:
            data.install_offline_policy(failed_root, ['com.apple.CommCenter'])
        except OSError:
            assert (failed_dir / 'disabled.plist').is_file()
            assert not (failed_dir / 'disabled.migrated').exists()
            chown.assert_not_called()
            cases.append({'case': 'sync failure aborts installation and retains incomplete files', 'result': 'pass'})
        else:
            raise AssertionError('Offline policy ignored sync failure')
    empty = data.install_offline_policy(out / 'absent-empty-policy', [])
    assert empty == dict(disabled_jobs=[], watchdogd_disabled=False, files=[])
    assert not (out / 'absent-empty-policy').exists()
    cases.append({'case': 'empty policy creates no filesystem objects', 'result': 'pass'})

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
    def mount_case(name, info, readonly, flags, reject=False, body_error=False):
        mountpoint = out / name
        attached = {'system-entities': [
            {'dev-entry': '/dev/disk42'},
            {'dev-entry': '/dev/disk43s1', 'mount-point': str(mountpoint)},
        ]}
        with mock.patch.object(data.subprocess, 'check_output', side_effect=[
                plistlib.dumps(attached), plistlib.dumps(info)]) as command, \
                mock.patch.object(data.os, 'statvfs', create=True,
                                  return_value=SimpleNamespace(f_flag=flags)) as statvfs, \
                mock.patch.object(data.os, 'ST_RDONLY', 1, create=True), \
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
            statvfs.assert_called_once_with(mountpoint)
            audit = json.loads(mountpoint.with_name(mountpoint.name + '-mount.json').read_text())
            assert audit['actual_readonly'] == bool(flags & 1)
            assert audit['requested_readonly'] == readonly and audit['diskutil'] == info
            attach = command.call_args_list[0].args[0]
            assert attach[attach.index('-owners') + 1] == 'on'
            assert ('-readonly' in attach) == readonly

    readonly_info = dict(FilesystemType='apfs', GlobalPermissionsEnabled=True)
    mount_case('source read-only without diskutil read-only key', readonly_info, True, 1)
    mount_case('source read-only with unrelated mount flags', readonly_info, True, 0x1009)
    mount_case('seed writable without diskutil read-only key', readonly_info, False, 0x1008)
    mount_case('source rejects writable mount', readonly_info, True, 0, reject=True)
    mount_case('seed rejects read-only mount', readonly_info, False, 1, reject=True)
    mount_case('diskutil cannot override actual writable state',
               {**readonly_info, 'WritableVolume': False, 'WritableMedia': False}, True, 0, reject=True)
    mount_case('diskutil cannot override actual read-only state',
               {**readonly_info, 'WritableVolume': True, 'WritableMedia': True}, True, 1)
    mount_case('seed rejects owners-off mount',
               {**readonly_info, 'GlobalPermissionsEnabled': False}, False, 0, reject=True)
    mount_case('seed rejects missing owners field', {'FilesystemType': 'apfs'}, False, 0, reject=True)
    mount_case('seed rejects wrong filesystem',
               {**readonly_info, 'FilesystemType': 'hfs'}, False, 0, reject=True)
    mount_case('seed failure still detaches', readonly_info, True, 1, body_error=True)
    report = dict(scope='Host format/config/content regression; no APFS mount or device validation',
                  host=os.name, source_sha256=data.sha256(ROOT / 'scripts/prepare-picasso-ram-data.py'),
                  native_posix_ownership_tested=os.name != 'nt',
                  offline_policy_ownership='simulated on every host; chown updates fixture metadata',
                  offline_policy_native_modes_tested=os.name != 'nt', cases=cases)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: {len(cases)} seed cases; outputs retained in {out}')


if __name__ == '__main__':
    main()
