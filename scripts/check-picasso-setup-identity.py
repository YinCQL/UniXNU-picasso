#!/usr/bin/env python3
"""Compare Setup handoff/Compose reuse with the pinned upstream packer's identity.

Sparse byte fixtures exercise the real parsers; they are not mounted APFS audits.
An optional failed Compose artifact is checked read-only at the handoff boundary.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--artifact', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    upstream = load('upstream_packer', args.packer)
    prepare = load('identity_prepare', ROOT / 'scripts/prepare-picasso-ram-data.py')
    checker = load('identity_checker', ROOT / 'scripts/check-picasso-data-artifacts.py')
    inputs = load('identity_inputs', ROOT / 'scripts/picasso_linux_inputs.py')
    trust_fixture = load('identity_trust_fixture', ROOT / 'scripts/check-picasso-ui-trust.py')
    spec = inputs.board()['payload']
    cases = []

    def rejected(name, operation, message):
        try:
            operation()
        except ValueError as error:
            assert message in str(error), (name, str(error))
        else:
            raise AssertionError('Accepted invalid identity: ' + name)
        cases.append(name)

    def check_handoff(work):
        base_path = work / 'ram-data/data.dbrdisk'
        base = prepare.inspect_packed(base_path)
        payload = json.loads((work / 'payload/payload.json').read_text())
        path, packed, transform = checker.payload_seed(work, spec, payload, base, prepare.inspect_packed)
        assert upstream.inspect(base_path)[0]['sha256'] == base['seed_identity_sha256']
        assert upstream.inspect(path)[0] == transform['output']
        assert packed['seed_identity_sha256'] == transform['output']['sha256']
        return base, packed, transform, path

    fixture = None
    for size in (2 * 1024**3, 2 * 1024**3 + 4096, 4 * 1024**3):
        work = out / str(size)
        (work / 'ram-data').mkdir(parents=True)
        (work / 'payload').mkdir()
        # Nonzero first/last pages and an explicitly encoded zero page: the
        # upstream identity must skip zeros while retaining the page indexes.
        def blob(value):
            return (struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, size, 2)
                    + struct.pack('<4Q', 0, 8192, size - 4096, 4096)
                    + bytes([value]) * 4096 + bytes(4096) + b'Z' * 4096)
        base_path = work / 'ram-data/data.dbrdisk'
        base_path.write_bytes(blob(65))
        key = hashlib.sha256(base_path.read_bytes()).hexdigest()[:12]
        key += '-' + hashlib.sha256(' '.join(spec['skip_setup_args']).encode()).hexdigest()[:8]
        setup_path = work / 'payload' / ('data-skip-setup-' + key + '.dbr')
        setup_path.write_bytes(blob(66))
        transform = dict(base_sha256=upstream.inspect(base_path)[0]['sha256'],
                         output=upstream.inspect(setup_path)[0], build=checker.firmware.BUILD, no_auto_dim=True)
        write_json(setup_path.with_suffix('.json'), transform)
        payload = dict(skip_setup=True, data_ramdisk_sha256=hashlib.sha256(setup_path.read_bytes()).hexdigest())
        write_json(work / 'payload/payload.json', payload)
        base, packed, transform, setup_path = check_handoff(work)
        assert (base['raw_sha256'] == base['seed_identity_sha256']) == (size <= 2 * 1024**3)
        cases.append('upstream Setup identity at ' + str(size))
        if size == 4 * 1024**3:
            fixture = work, base, packed, transform, setup_path, payload

    work, base, packed, transform, setup_path, payload = fixture
    # Reuse the already parsed bytes while mutating only transformation reports.
    inspect_cached = lambda path: packed
    def wrong_report(name, value, message):
        write_json(setup_path.with_suffix('.json'), value)
        rejected(name, lambda: checker.payload_seed(work, spec, payload, base, inspect_cached), message)
        write_json(setup_path.with_suffix('.json'), transform)
    wrong_report('reject full-byte base hash above 2 GiB', {**transform, 'base_sha256': base['raw_sha256']}, 'wrong base')
    wrong_report('reject full-byte output hash above 2 GiB',
                 {**transform, 'output': {**transform['output'], 'sha256': packed['raw_sha256']}}, 'report mismatch')
    wrong_report('reject packed-byte base hash', {**transform, 'base_sha256': base['packed_sha256']}, 'wrong base')

    # Execute validate() end to end with a structural audit fixture. The audit
    # does not claim the fixture contains APFS; no parser/identity check is mocked.
    tree = out / 'upstream'
    packer = tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'
    packer.parent.mkdir(parents=True)
    shutil.copyfile(args.packer, packer)
    helper = tree / 'scripts/seed-data-skip-setup.py'
    helper.parent.mkdir()
    helper.write_text('# recipe fixture; not executed\n')
    directory = out / 'inputs'
    directory.mkdir()
    shutil.copyfile(work / 'ram-data/data.dbrdisk', directory / 'data.dbrdisk')
    shutil.copyfile(setup_path, directory / 'setup.dbr')
    write_json(directory / 'setup.json', transform)
    write_json(directory / 'source-var-manifest.json', {'fixture': True})
    jobs = prepare.offline_jobs(spec)
    policy = dict(disabled_jobs=jobs, watchdogd_disabled='com.apple.watchdogd' in jobs, files=[])
    for name, value in prepare.offline_policy_contents(jobs).items():
        policy['files'].append(dict(path=prepare.OFFLINE_POLICY_DIR + '/' + name, type='file', uid=0, gid=0,
                                    mode=0o644, bytes=len(value), sha256=hashlib.sha256(value).hexdigest()))
    identity = '12345678-1234-4678-9234-567812345678'
    seed = dict(base, schema='picasso-ram-data-v1', volume_uuid=identity, source_sha256=prepare.OS_SHA256,
                source_launchd_sha256=prepare.LAUNCHD_SHA256, persistent=False, ownership_preserved=True,
                os_inputs_modified=False, packer_sha256=inputs.digest(packer),
                source_manifest_sha256=inputs.digest(directory / 'source-var-manifest.json'), offline_launch_policy=policy)
    write_json(directory / 'seed.json', seed)
    library = trust_fixture.fixture_library()
    (directory / 'libpicasso-sb.dylib').write_bytes(library)
    trust = inputs.load_script('picasso_ui_trust')
    library_identity = trust.library_identity(library)
    audit = dict(packed=base, payload_packed=packed, payload_seed_transformation=transform, volume_uuid=identity,
                 persistent=True, encrypted=False, offline_launch_policy=policy, payload_offline_launch_policy=policy,
                 payload_setup_settings=dict(preferences=['com.apple.migration', 'com.apple.purplebuddy',
                     'com.apple.springboard', 'com.apple.UIKit'], injected_library=dict(
                         path='picasso/usr-lib/libpicasso-sb.dylib', sha256=library_identity['sha256'])),
                 ui_trust=dict(library=library_identity, original_modules_unchanged=True, owned_entries=1,
                               original_bundle_sha256=trust.TRUST_BUNDLE_SHA256))
    write_json(directory / 'data-audit.json', audit)
    def manifest():
        write_json(directory / 'manifest.json', dict(schema=inputs.SCHEMA, recipe_sha256=inputs.recipe(tree),
            files={n: dict(bytes=(directory / n).stat().st_size, sha256=inputs.digest(directory / n)) for n in inputs.FILES}))
    manifest()
    assert inputs.validate(directory, tree)['volume_uuid'] == identity
    cases.append('4 GiB Compose reuse validates all hashes and handoff fields')
    for name, bad, message in (
        ('reuse rejects wrong base hash domain', {**transform, 'base_sha256': base['raw_sha256']}, 'another base'),
        ('reuse rejects wrong output hash domain', {**transform, 'output': {**transform['output'], 'sha256': packed['raw_sha256']}}, 'hash/geometry mismatch'),
    ):
        write_json(directory / 'setup.json', bad)
        manifest()
        rejected(name, lambda: inputs.validate(directory, tree), message)
    write_json(directory / 'setup.json', transform)
    manifest()

    actual = None
    if args.artifact:
        artifact = args.artifact.resolve()
        base, packed, transform, path = check_handoff(artifact)
        seed = json.loads((artifact / 'ram-data/seed.json').read_text())
        assert all(seed[k] == v for k, v in base.items())
        uuid = seed['volume_uuid']
        assert f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{uuid}"' in (artifact / '.config').read_text()
        kernel = artifact / 'kernel/kernel.macho'
        plan = json.loads(kernel.with_suffix('.json').read_text())
        assert checker.digest(kernel) == plan['candidate_sha256'] and uuid.encode() + b'\0' in kernel.read_bytes()
        payload = artifact / 'payload/payload.img'
        report = json.loads(payload.with_suffix('.json').read_text())
        assert checker.digest(payload) == report['payload_sha256'] and report['kernel_sha256'] == plan['candidate_sha256']
        actual = dict(base=base, setup=packed, transformation=transform, volume_uuid=uuid,
                      kernel_sha256=plan['candidate_sha256'], payload_sha256=report['payload_sha256'])
        cases.append('original failed Compose handoff, UUID and artifact hashes')
    write_json(out / 'results.json', dict(passed=True, cases=cases, artifact=actual,
        packer_sha256=inputs.digest(args.packer), apfs_mounted=False, hardware_tested=False,
        source_sha256={name: inputs.digest(ROOT / name) for name in (
            'scripts/check-picasso-setup-identity.py', 'scripts/check-picasso-data-artifacts.py',
            'scripts/picasso_linux_inputs.py', 'scripts/prepare-picasso-ram-data.py')}))
    print('PASS Setup identity:', len(cases), 'cases; upstream hashes and reuse checked')


if __name__ == '__main__':
    main()
