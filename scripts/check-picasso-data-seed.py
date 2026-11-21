#!/usr/bin/env python3
"""Exercise seed parameter, APFS identity and input preservation boundaries."""
import argparse
import copy
import json
import os
from pathlib import Path
import plistlib
import stat
import struct
import subprocess
import sys
import tempfile
from unittest import mock
import uuid

import picasso_data_seed as seed


def policy_cases(work, cases, reject):
    prepare = seed.legacy.load_script('prepare-picasso-ram-data')
    defaults = prepare.offline_jobs(seed.legacy.board()['payload'])
    default_request = seed.options('20', '', 'true', '90')
    assert 'offline_launch_policy' not in default_request
    assert seed.validate_request(default_request) == defaults
    assert seed.legacy_compatible(default_request, defaults)
    cases.append('unchanged-default-request-and-legacy-policy')
    custom = ['com.apple.CommCenter', 'com.apple.watchdogd']
    policy_input = json.dumps({'disabled_jobs': custom})
    request = seed.options('20', '', 'true', '90', policy_input)
    assert request['offline_launch_policy'] == {'disabled_jobs': custom}
    assert seed.validate_request(request) == custom
    assert not seed.legacy_compatible(request, custom)
    cases.append('custom-policy-binds-request-and-disables-legacy-claim')
    empty = seed.options('20', '', 'true', '90', '{"disabled_jobs":[]}')
    assert seed.validate_request(empty) == [] and not seed.legacy_compatible(empty, [])
    same = seed.options('20', '', 'true', '90', json.dumps({'disabled_jobs': defaults}))
    assert seed.legacy_compatible(same, seed.validate_request(same))
    cases.append('explicit-empty-and-explicit-default-policy')
    removed = seed.options('20', '', 'true', '90', enable='["com.apple.bluetoothd"]')
    assert seed.validate_request(removed) == [job for job in defaults if job != 'com.apple.bluetoothd']
    added = seed.options('20', '', 'true', '90', '{"disabled_jobs":[]}', '["com.apple.watchdogd"]')
    assert seed.validate_request(added) == ['com.apple.watchdogd']
    patched = seed.options('20', '', 'true', '90', policy_input, '["com.apple.ReportCrash"]', '["com.apple.CommCenter"]')
    assert seed.validate_request(patched) == ['com.apple.watchdogd', 'com.apple.ReportCrash']
    assert seed.requested_jobs(policy_input, '["com.apple.watchdogd"]') == custom
    cases.append('add-remove-and-idempotent-add-use-selected-list')
    for value in ('null', '[]', '{"files":[]}', '{"disabled_jobs":[],"files":[]}',
                  '{"disabled_jobs":"com.apple.CommCenter"}', '{"disabled_jobs":[null]}',
                  '{"disabled_jobs":["com.apple.CommCenter","com.apple.CommCenter"]}', '{', 'x'*16385):
        reject('bad-policy-' + repr(value[:80]), lambda value=value: seed.requested_jobs(value))
    for value in ('null', '{}', '[1]', '["org.example.daemon"]', '["com.apple."]',
                  '["com.apple.job with space"]', '["com.apple.job\\n"]', '["com.apple.$(id)"]',
                  '["com.apple../job"]', '["com.apple.a","com.apple.a"]',
                  json.dumps(['com.apple.' + 'a'*256]),
                  json.dumps(['com.apple.job' + str(i) for i in range(257)])):
        reject('bad-job-array-' + repr(value[:80]), lambda value=value: seed.requested_jobs(disable=value))
        reject('bad-enable-array-' + repr(value[:80]), lambda value=value: seed.requested_jobs(enable=value))
    for job in prepare.PROTECTED_JOBS:
        reject('protected-custom-' + job, lambda job=job: seed.requested_jobs(json.dumps({'disabled_jobs': [job]})))
        reject('protected-add-' + job, lambda job=job: seed.requested_jobs(disable=json.dumps([job])))
    reject('overlapping-patches', lambda: seed.requested_jobs(policy_input, '["com.apple.CommCenter"]', '["com.apple.CommCenter"]'))
    reject('enable-not-in-selected-list', lambda: seed.requested_jobs('{"disabled_jobs":[]}', enable='["com.apple.bluetoothd"]'))
    reject('effective-policy-too-large', lambda: seed.requested_jobs(disable=json.dumps(['com.apple.job' + str(i) for i in range(256)])))
    bad_request = copy.deepcopy(request)
    bad_request['offline_launch_policy']['files'] = []
    reject('request-cannot-provide-file-audit', lambda: seed.validate_request(bad_request))
    reject('extra-request-field', lambda: seed.validate_request({**request, 'enable_jobs': []}))

    # Exercise the generator's prepare hook with real policy writes and readback.
    # Unprivileged hosts model root ownership; POSIX permissions and file bytes
    # remain real. Root runs additionally verify native ownership below.
    root = work / 'policy-data'
    directory = root / prepare.OFFLINE_POLICY_DIR
    directory.mkdir(parents=True)
    for path in (root, root / 'db', directory):
        os.chmod(path, 0o755)
    owners = {path: (0, 0) for path in (root, root / 'db', directory)}
    actual_metadata = prepare.metadata
    def metadata(path):
        entry = actual_metadata(path)
        entry['uid'], entry['gid'] = owners.get(Path(path), (501, 20))
        if os.name == 'nt':
            entry['mode'] = 0o755 if entry['type'] == 'directory' else 0o644
        return entry
    def chown(path, uid, gid, *, follow_symlinks):
        assert follow_symlinks is False
        owners[Path(path)] = (uid, gid)
    original_jobs = prepare.offline_jobs
    def prepare_fixture(tree, source, output, config):
        jobs = prepare.offline_jobs(seed.legacy.board()['payload'])
        assert jobs == custom
        return prepare.install_offline_policy(root, jobs)
    with mock.patch.object(prepare, 'metadata', side_effect=metadata), \
            mock.patch.object(prepare.os, 'chown', side_effect=chown, create=True), \
            mock.patch.object(prepare, 'prepare', side_effect=prepare_fixture):
        seed.prepare_base(prepare, None, None, None, None, custom)
        assert prepare.offline_jobs is original_jobs
        actual = seed.verify_seed_policy(prepare, root, custom)
        assert plistlib.loads((directory / 'disabled.plist').read_bytes()) == {job: True for job in custom}
        assert (directory / 'disabled.migrated').read_bytes() == b''
        assert actual == seed.expected_policy(prepare, custom)
        reject('empty-policy-cannot-retain-files', lambda: seed.verify_seed_policy(prepare, root, []))
        reports = dict(offline_launch_policy=actual, payload_offline_launch_policy=actual)
        seed.verify_policy_reports(prepare, custom, reports, reports)
        reject('custom-report-validated-as-default', lambda: seed.verify_policy_reports(prepare, defaults, reports, reports))
        for field in ('offline_launch_policy', 'payload_offline_launch_policy'):
            changed = copy.deepcopy(reports)
            changed[field]['disabled_jobs'] = []
            reject('policy-report-mismatch-' + field, lambda changed=changed: seed.verify_policy_reports(prepare, custom, reports, changed))
        (directory / 'disabled.plist').write_bytes(plistlib.dumps({'com.apple.CommCenter': True}))
        reject('real-policy-content-mismatch', lambda: prepare.verify_offline_policy(root, custom))
    cases.append('prepare-hook-real-plist-marker-and-report-binding-with-simulated-owner')
    with mock.patch.object(prepare, 'prepare', side_effect=ValueError('fixture preparation failure')):
        reject('prepare-hook-propagates-failure', lambda: seed.prepare_base(prepare, None, None, None, None, custom))
    assert prepare.offline_jobs is original_jobs
    cases.append('prepare-hook-restores-policy-function-on-failure')
    assert prepare.install_offline_policy(work / 'empty-policy', []) == seed.expected_policy(prepare, [])
    assert seed.verify_seed_policy(prepare, work / 'empty-policy', []) == seed.expected_policy(prepare, [])
    assert not (work / 'empty-policy').exists()
    cases.append('empty-policy-writes-no-overrides')
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        native = work / 'native-policy'
        (native / prepare.OFFLINE_POLICY_DIR).mkdir(parents=True)
        actual = prepare.install_offline_policy(native, custom)
        assert actual == seed.expected_policy(prepare, custom)
        for name in ('disabled.plist', 'disabled.migrated'):
            info = (native / prepare.OFFLINE_POLICY_DIR / name).stat()
            assert (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) == (0, 0, 0o644)
        cases.append('native-posix-root-policy-ownership-and-modes')
    environment = dict(os.environ, DATA_SIZE_GIB='20', DATA_SIZE_MIB='', USERDATA_BYTES='',
                       DATA_UUID='', SKIP_SETUP='true', RETENTION_DAYS='90',
                       OFFLINE_LAUNCH_POLICY=policy_input, DISABLE_JOBS='["com.apple.ReportCrash"]',
                       ENABLE_JOBS='["com.apple.CommCenter"]', GITHUB_OUTPUT='')
    output = work / 'cli-request.json'
    result = subprocess.run([sys.executable, '-B', str(Path(seed.__file__)), 'options', '--output', str(output)],
                            env=environment, text=True, capture_output=True, check=True)
    assert json.loads(output.read_text()) == patched
    assert json.loads(result.stdout) == patched
    cases.append('workflow-env-reaches-canonical-request')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--policy-only', action='store_true', help='Check parameters and offline policy without a prepared tree')
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
    for size in ('4', '20', '46', '46.125', '110', '238', '1024'):
        for skip in ('true', 'false'):
            for identity in ('', requested.lower()):
                value = seed.options(size, identity, skip, '90')
                assert value['data_uuid'] == (requested if identity else '')
                cases.append(f'parameters-{size}-{skip}-{bool(identity)}')
    for size in ('0', '3', '1025', '4.00001', '04', '-4', '20\n', '$(id)'):
        reject('bad-size-' + repr(size), lambda: seed.options(size, '', 'true', '90'))
    for identity in ('0' * 32, '00000000-0000-0000-0000-000000000000', requested + '\n', '$(id)', '../seed'):
        reject('bad-uuid-' + repr(identity), lambda: seed.options('20', identity, 'true', '90'))
    for days in ('0', '91', '1.5', '-1', '7\n'):
        reject('bad-retention-' + repr(days), lambda: seed.options('20', '', 'true', days))
    reject('bad-boolean', lambda: seed.options('20', '', 'yes', '90'))
    with tempfile.TemporaryDirectory(prefix='picasso-seed-host-') as temporary:
        work = Path(temporary)
        policy_cases(work, cases, reject)
        if args.policy_only:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            seed.write_json(args.output, dict(passed=True, cases=cases,
                scope='host parameter/policy fixtures; no APFS mount or kernel qualification',
                native_policy_ownership_tested=hasattr(os, 'geteuid') and os.geteuid() == 0))
            print(f'PASS: {len(cases)} seed parameter and policy boundaries')
            return
        if args.tree is None:
            parser.error('--tree is required unless --policy-only is selected')
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
    seed.write_json(args.output, dict(passed=True, cases=cases, scope='host fixtures; no APFS mount or kernel qualification',
                    native_policy_ownership_tested=hasattr(os, 'geteuid') and os.geteuid() == 0))
    print(f'PASS: {len(cases)} seed generation boundaries')


if __name__ == '__main__':
    main()
