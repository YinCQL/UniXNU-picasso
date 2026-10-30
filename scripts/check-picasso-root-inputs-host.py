#!/usr/bin/env python3
"""Host regression checks for record verification and read-only attach cleanup.

Native hdiutil is modeled; optional real root-image bytes are read from a file.
No macOS, SDK or phone validation is claimed by this test.
"""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
from unittest.mock import patch

import picasso_target as target

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root-image', type=Path)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    path = ROOT / 'scripts/check-picasso-root-inputs.py'
    spec = importlib.util.spec_from_file_location('root_inputs', path)
    check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)
    passed = []
    def reject(name, operation):
        try: operation()
        except (ValueError, OSError): passed.append(name)
        else: raise AssertionError('Invalid root input accepted: ' + name)

    data = b'A' * 131072 + b'B' * 131072 + bytes(range(37))
    expected = dict(name='fixture', size=len(data), sha256=hashlib.sha256(data).hexdigest())
    manifest = bytes(544) + b''.join(hashlib.sha256(data[i:i+131072]).digest() for i in range(0, len(data), 131072))
    result = check.verify_stream(io.BytesIO(data), expected, manifest)
    assert result['signed_records_checked'] == 3
    passed.append('full and partial records')
    class ShortReads(io.BytesIO):
        def read(self, size): return super().read(min(size, 7001))
    assert check.verify_stream(ShortReads(data), expected, manifest) == result
    passed.append('short non-EOF reads assembled')
    reject('early EOF', lambda: check.verify_stream(io.BytesIO(data[:-1]), expected, manifest))
    reject('changed record', lambda: check.verify_stream(io.BytesIO(b'Z' + data[1:]), expected, manifest))
    reject('record order', lambda: check.verify_stream(io.BytesIO(data[131072:262144] + data[:131072] + data[262144:]), expected, manifest))
    reject('manifest length', lambda: check.verify_stream(io.BytesIO(data), expected, manifest[:-1]))
    reject('whole-disk hash', lambda: check.verify_stream(io.BytesIO(data), dict(expected, sha256='00' * 32), manifest))

    def lifecycle(injected=False, mounted=False, synthesized=False):
        calls = []
        entities = [{'dev-entry': '/dev/disk999'}, {'dev-entry': '/dev/disk999s1'}]
        if synthesized: entities.append({'dev-entry': '/dev/disk1000'})
        if mounted: entities[1]['mount-point'] = '/unexpected'
        def output(command):
            calls.append(command)
            assert command[:2] == ['hdiutil', 'attach']
            assert all(flag in command for flag in ('-readonly', '-nomount', '-noautofsck', '-plist'))
            return plistlib.dumps({'system-entities': entities})
        def detach(command, **kwargs):
            calls.append(command)
            assert command == ['hdiutil', 'detach', '/dev/disk999'] and kwargs == {'check': True}
        with patch.object(check.subprocess, 'check_output', output), patch.object(check.subprocess, 'run', detach):
            try:
                with check.attached_disk(Path('fixture.dmg')) as raw:
                    assert raw == '/dev/rdisk999'
                    if injected: raise OSError('injected read failure')
            except (OSError, ValueError):
                assert injected or mounted
            else: assert not injected and not mounted
        assert len(calls) == 2
    lifecycle(); passed.append('readonly whole-disk attachment and detach')
    lifecycle(injected=True); passed.append('detach after read failure')
    lifecycle(mounted=True); passed.append('unexpected mount rejected and detached')
    lifecycle(synthesized=True); passed.append('whole image selected before synthesized APFS container')

    # Exercise the real failure-report path before any hdiutil invocation.
    failure = out / 'missing-input-report.json'
    with patch.object(check.sys, 'platform', 'darwin'):
        reject('missing input report retained', lambda: check.check(out / 'missing-inputs', failure))
        before = failure.read_bytes()
        reject('existing report retained', lambda: check.check(out, failure))
        assert before == failure.read_bytes()
    assert json.loads(failure.read_text())['passed'] is False
    assert json.loads(failure.read_text())['native_hdiutil'] is False

    actual = []
    if args.root_image:
        assert args.root_image.stat().st_size == target.ROOT_IMAGE_BYTES
        with args.root_image.open('rb') as stream:
            for window in target.ROOT_WINDOWS:
                stream.seek(window['offset'])
                manifest_path = ROOT / 'policy/verified-root-picasso' / window['directory'] / 'manifest.bin'
                actual.append(check.verify_stream(stream, window, manifest_path.read_bytes()))
    report = dict(passed=True, scope=__doc__, checks=passed, real_file_windows=actual,
                  native_hdiutil_executed=False, source_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS:', len(passed), 'host checks;', len(actual), 'real file windows; native hdiutil not executed')


if __name__ == '__main__':
    main()
