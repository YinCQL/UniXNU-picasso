#!/usr/bin/env python3
"""Regression for base/Setup seed handoff; APFS mounts and ownership are models."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import stat
import struct
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    data = load('seed_prepare', ROOT / 'scripts/prepare-picasso-ram-data.py')
    check = load('seed_artifacts', ROOT / 'scripts/check-picasso-data-artifacts.py')
    wrapper = load('seed_wrapper', ROOT / 'scripts/picasso-seed-skip-setup.py')
    check.ROOT = out / 'device'
    work = out / 'work'
    (work / 'ram-data').mkdir(parents=True)
    (work / 'payload').mkdir()
    cases = []

    def case(name, operation, error=None):
        try:
            result = operation()
        except ValueError as failure:
            assert error and error in str(failure), (name, str(failure))
            result = {'rejected': str(failure)}
        else:
            assert error is None, (name, 'unexpected acceptance')
        cases.append({'case': name, 'result': 'pass' if error is None else 'rejected as expected'})
        return result

    # Independently encoded specimens; the Setup copy differs from its base.
    def specimen(content):
        raw = content + bytes(4 * 4096 - len(content))
        blob = (struct.pack('<8sIIQQQQ', b'DBRDISK1', 1, 4096, len(raw), 1, 0, len(content))
                + content)
        return raw, blob, dict(logical_bytes=len(raw), packed_bytes=len(blob),
                              raw_sha256=hashlib.sha256(raw).hexdigest(),
                              packed_sha256=hashlib.sha256(blob).hexdigest(),
                              page_size=4096, extent_count=1)

    _, base_blob, base = specimen(b'A' * 4096)
    raw, blob, packed = specimen(b'B' * 4096)
    (work / 'ram-data/data.dbrdisk').write_bytes(base_blob)
    spec = dict(skip_setup_tool='scripts/picasso-seed-skip-setup.py',
                skip_setup_args=['--no-auto-dim', '--inject-lib', 'out/picasso/libpicasso-sb.dylib',
                                 '--statusbar-provider', '_UIStatusBarVisualProvider_Pad_ForcedNoCell'])
    path = work / 'payload' / ('data-skip-setup-' + base['packed_sha256'][:12] + '-'
                              + hashlib.sha256(' '.join(spec['skip_setup_args']).encode()).hexdigest()[:8] + '.dbr')
    path.write_bytes(blob)
    transform = dict(base_sha256=base['raw_sha256'], build=check.firmware.BUILD, no_auto_dim=True,
                     output=dict(bytes=len(raw), sha256=packed['raw_sha256'], packed=True,
                                 encoded_bytes=len(blob), encoded_sha256=packed['packed_sha256']))
    report_path = path.with_suffix('.json')
    report_path.write_text(json.dumps(transform))
    report = dict(skip_setup=True, data_ramdisk_sha256=packed['packed_sha256'])
    inspect = data.inspect_packed
    selected = case('Setup copy may differ from validated base',
                    lambda: check.payload_seed(work, spec, report, base, inspect))
    assert selected == (path, packed, transform) and packed['packed_sha256'] != base['packed_sha256']
    selected = case('plain payload still requires original bytes', lambda: check.payload_seed(
        work, spec, dict(skip_setup=False, data_ramdisk_sha256=base['packed_sha256']), base, inspect))
    assert selected == (work / 'ram-data/data.dbrdisk', base, None)
    case('plain payload rejects Setup bytes', lambda: check.payload_seed(
        work, spec, {**report, 'skip_setup': False}, base, inspect), 'Wrong payload seed')
    case('Setup flag must be boolean', lambda: check.payload_seed(
        work, spec, {**report, 'skip_setup': 'true'}, base, inspect), 'Invalid payload Setup mode')
    case('reject unconfigured Setup tool', lambda: check.payload_seed(
        work, {**spec, 'skip_setup_tool': 'other.py'}, report, base, inspect), 'Unexpected payload Setup tool')
    case('reject stale arguments cache', lambda: check.payload_seed(
        work, {**spec, 'skip_setup_args': []}, report, base, inspect), 'Missing payload Setup seed')
    case('reject unrelated payload seed', lambda: check.payload_seed(
        work, spec, {**report, 'data_ramdisk_sha256': '0' * 64}, base, inspect), 'Wrong payload Setup seed')

    def altered_report(name, value, expected, label=None):
        changed = {**transform, name: value}
        report_path.write_text(json.dumps(changed))
        try:
            case('reject report ' + (label or name),
                 lambda: check.payload_seed(work, spec, report, base, inspect), expected)
        finally:
            report_path.write_text(json.dumps(transform))

    altered_report('base_sha256', '0' * 64, 'wrong base')
    altered_report('build', '21H461', 'wrong iOS build')
    altered_report('no_auto_dim', False, 'dim policy differs')
    for field in ('bytes', 'sha256', 'encoded_bytes', 'encoded_sha256', 'packed'):
        value = False if field == 'packed' else (0 if field.endswith('bytes') else '0' * 64)
        altered_report('output', {**transform['output'], field: value},
                       'must remain packed' if field == 'packed' else 'report mismatch', field)
    path.write_bytes(blob[:-1] + b'C')
    case('reject changed packed bytes', lambda: check.payload_seed(work, spec, report, base, inspect), 'report mismatch')
    path.write_bytes(blob)
    _, bigger_blob, _ = specimen(b'C' * 8192)
    bigger_blob = bytearray(bigger_blob)
    struct.pack_into('<Q', bigger_blob, 16, 5 * 4096)
    path.write_bytes(bigger_blob)
    changed = inspect(path)
    report_path.write_text(json.dumps({**transform, 'output': dict(
        bytes=changed['logical_bytes'], sha256=changed['raw_sha256'], packed=True,
        encoded_bytes=changed['packed_bytes'], encoded_sha256=changed['packed_sha256'])}))
    case('reject capacity change even with matching report', lambda: check.payload_seed(
        work, spec, {**report, 'data_ramdisk_sha256': changed['packed_sha256']}, base, inspect), 'changed Data capacity')
    path.write_bytes(blob)
    report_path.write_text(json.dumps(transform))

    identity = '12345678-1234-5678-1234-567812345678'
    jobs = ['com.apple.CommCenter']
    library = check.ROOT / spec['skip_setup_args'][spec['skip_setup_args'].index('--inject-lib') + 1]
    library.parent.mkdir(parents=True)
    library.write_bytes(b'independent-library-fixture')
    # Exercise the wrapper's actual merge hook with independent plist I/O.
    seed_root = out / 'wrapper-seed'
    prefs = seed_root / 'mobile/Library/Preferences'
    prefs.mkdir(parents=True)
    ui_path = prefs / 'com.apple.UIKit.plist'
    ui_path.write_bytes(plistlib.dumps({'ExistingPreference': 42}))
    merged = []

    def merge_pref(var, name, values):
        destination = var / 'mobile/Library/Preferences' / (name + '.plist')
        previous = plistlib.loads(destination.read_bytes()) if destination.exists() else {}
        destination.write_bytes(plistlib.dumps({**previous, **values}, fmt=plistlib.FMT_BINARY))
        merged.append(name)

    def wrapper_readback():
        arguments = ['seed.py', 'base.dbr', 'output.dbr', *spec['skip_setup_args']]
        provider = wrapper.consume_statusbar_provider(arguments)
        assert arguments == ['seed.py', 'base.dbr', 'output.dbr', *spec['skip_setup_args'][:3]]
        tool = SimpleNamespace(merge_pref=merge_pref)
        wrapper.configure_seed(tool, library, provider)
        tool.merge_pref(seed_root, 'com.apple.migration', {'LastSystemVersion': check.firmware.BUILD})
        tool.merge_pref(seed_root, 'com.apple.springboard', {'SBDisableAutoDim': True})
        assert plistlib.loads(ui_path.read_bytes()) == {
            'ExistingPreference': 42, 'UIStatusBarVisualProviderClassName': '_UIStatusBarVisualProvider_Pad_ForcedNoCell',
            'StatusBarHidesDate': True}
        assert merged.count('com.apple.UIKit') == 1
        assert plistlib.loads((prefs / 'com.apple.springboard.plist').read_bytes()) == {
            'SBDisableAutoDim': True, 'BSUIVibrancyEffectViewDisabled': True}
        assert (seed_root / 'picasso/libpicasso-sb.dylib').read_bytes() == library.read_bytes()

    case('wrapper writes UIKit suite and preserves existing preferences', wrapper_readback)
    assert wrapper.consume_statusbar_provider(['seed.py', 'base', 'output']) is None
    for arguments in (['--statusbar-provider'], ['--statusbar-provider', '_UIStatusBarFallbackVisualProvider'],
                      ['--statusbar-provider', wrapper.STATUSBAR_PROVIDER, '--statusbar-provider', wrapper.STATUSBAR_PROVIDER]):
        case('reject wrapper provider ' + ' '.join(arguments),
             lambda arguments=arguments: wrapper.consume_statusbar_provider(list(arguments)),
             'Duplicate' if arguments.count('--statusbar-provider') > 1 else 'reviewed rectangular provider')
    state = {}

    def metadata(path):
        path = Path(path)
        mode = path.lstat().st_mode
        result = dict(type='directory' if stat.S_ISDIR(mode) else 'file', uid=0, gid=0,
                      mode=0o755 if stat.S_ISDIR(mode) else 0o644)
        if stat.S_ISREG(mode):
            result.update(bytes=path.stat().st_size, sha256=check.digest(path))
        return result

    @contextmanager
    def mounted(image, target, readonly):
        assert readonly is True and image.read_bytes() == raw
        target.mkdir(parents=True)
        directory = target / data.OFFLINE_POLICY_DIR
        directory.mkdir(parents=True)
        for name, content in data.offline_policy_contents(jobs).items():
            (directory / name).write_bytes(content)
        if state.get('bad_policy'):
            (directory / 'disabled.plist').write_bytes(plistlib.dumps({'com.apple.notifyd': True}))
        preferences = target / 'mobile/Library/Preferences'
        preferences.mkdir(parents=True)
        for name, values in {
            'com.apple.migration': dict(LastSystemVersion=state.get('build', check.firmware.BUILD),
                DMUserDataDisposition=dict(SyntheticDidUpgrade=False, buildVersion=check.firmware.BUILD)),
            'com.apple.purplebuddy': dict(SetupDone=not state.get('bad_setup'), SetupFinishedAllSteps=True,
                                         SetupVersion=11, daemonizeNeedsToRun=False),
            'com.apple.springboard': dict(BSUIVibrancyEffectViewDisabled=True, SBDisableAutoDim=True,
                                         SBDontDimOrLockOnAC=True, SBDisableIdleSleep=True),
            'com.apple.UIKit': dict(UIStatusBarVisualProviderClassName=state.get(
                'provider', '_UIStatusBarVisualProvider_Pad_ForcedNoCell'), StatusBarHidesDate=not state.get('show_date')),
        }.items():
            if name == 'com.apple.UIKit' and state.get('missing_ui_domain'):
                continue
            if name == 'com.apple.UIKit' and state.get('missing_provider'):
                values = {}
            (preferences / (name + '.plist')).write_bytes(plistlib.dumps(values))
        destination = target / 'picasso/libpicasso-sb.dylib'
        destination.parent.mkdir()
        destination.write_bytes(b'wrong-library' if state.get('bad_library') else library.read_bytes())
        destination.chmod(0o644)
        yield dict(VolumeUUID=state.get('uuid', identity))

    policy = dict(disabled_jobs=jobs, watchdogd_disabled=False, files=[
        dict(path=data.OFFLINE_POLICY_DIR + '/' + name, type='file', uid=0, gid=0, mode=0o644,
             bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
        for name, content in data.offline_policy_contents(jobs).items()])
    preparation = dict(inspect_packed=inspect, mounted=mounted, verify_offline_policy=data.verify_offline_policy)
    native_lstat = Path.lstat

    def modeled_lstat(path):
        result = native_lstat(path)
        if path.name == 'libpicasso-sb.dylib':
            # Windows exposes DOS read-only state rather than POSIX mode bits.
            mode = stat.S_IFREG | (0o600 if state.get('bad_library_mode') else 0o644)
            return os.stat_result((mode, *tuple(result)[1:]))
        return result

    def policy_case(name, expected=None):
        with patch.object(data, 'metadata', metadata), patch.object(Path, 'lstat', modeled_lstat):
            return case(name, lambda: check.verify_payload_policy(path, packed, preparation,
                        identity, spec, jobs, policy, out / (name + '.json')), expected)

    result = policy_case('readonly policy and Setup readback model')
    assert result[0] == policy and result[1]['injected_library']['sha256'] == check.digest(library)
    for name, value, error in (
        ('uuid', '00000000-0000-0000-0000-000000000001', 'Data UUID mismatch'),
        ('bad_policy', True, 'Offline policy content'),
        ('build', '21H461', 'Setup preferences differ'),
        ('bad_setup', True, 'Setup preferences differ'),
        ('provider', '_UIStatusBarFallbackVisualProvider', 'Setup preferences differ: com.apple.UIKit'),
        ('show_date', True, 'Setup preferences differ: com.apple.UIKit'),
        ('missing_provider', True, 'Setup preferences differ: com.apple.UIKit'),
        ('missing_ui_domain', True, 'Missing Setup preference: com.apple.UIKit'),
        ('bad_library', True, 'library differs'),
        ('bad_library_mode', True, 'library metadata'),
    ):
        state.clear()
        state[name] = value
        policy_case('reject mounted ' + name, error)
    result = dict(scope=__doc__, passed=len(cases), cases=cases, hardware_tested=False,
                  apfs_mounted=False, sdk_compiled=False)
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
