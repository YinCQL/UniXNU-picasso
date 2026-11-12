#!/usr/bin/env python3
"""Generate, verify and stage iPhone X / 20H392 Data seeds without building a kernel.

Generate requires native macOS and root; validate/stage also run on Linux.
The existing Compose recipe and previously retained bundles remain unchanged.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import struct
import subprocess
import sys
import uuid

import picasso_linux_inputs as legacy
import picasso_target as target
import picasso_ui_trust as trust

ROOT = legacy.ROOT
SCHEMA = 'picasso-data-seed-v1'
WORKFLOW = '.github/workflows/generate-data-seed.yml'
FILES = (*legacy.FILES, 'ui-trust.bundle', 'seed-config.txt')
GIB = 1024**3
PAGE = 4096


def require(value, message):
    if not value:
        raise ValueError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def options(size, identity, skip, retention):
    require(re.fullmatch(r'(?:[4-9]|1[0-9]|20)', str(size)), 'Data capacity must be an integer from 4 to 20 GiB')
    require(re.fullmatch(r'(?:[1-9]|[1-8][0-9]|90)', str(retention)), 'Retention must be an integer from 1 to 90 days')
    require(skip in ('true', 'false'), 'skip_setup must be true or false')
    if identity:
        require(re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', identity), 'Invalid Data UUID')
        require(uuid.UUID(identity).int != 0, 'Data UUID cannot be zero')
        identity = str(uuid.UUID(identity)).upper()
    return dict(data_size_gib=int(size), data_uuid=identity, skip_setup=skip == 'true', retention_days=int(retention))


def checksum(block):
    """APFS object Fletcher64, excluding the first eight checksum bytes."""
    require(len(block) == PAGE, 'APFS object must use 4 KiB blocks')
    mod = 0xffffffff
    first = second = 0
    for (word,) in struct.iter_unpack('<I', block[8:]):
        first = (first + word) % mod
        second = (second + first) % mod
    low = mod - (first + second) % mod
    high = mod - (first + low) % mod
    return struct.pack('<II', low, high)


def regions(path, tree):
    return runpy.run_path(str(tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'))['regions'](path.read_bytes())


def volume_blocks(path, tree):
    """Accept one unencrypted, snapshot-free APFS volume and its checkpoints.

    apfs_superblock_t: magic +32, object type +24, UUID +240, flags +264.
    Layout follows Apple's APFS reference; runtime.seed_uuid uses the same UUID.
    """
    _, extents = regions(path, tree)
    found = []
    for offset, data in extents:
        for at in range(0, len(data), PAGE):
            block = bytes(data[at:at + PAGE])
            if block[32:36] != b'APSB':
                continue
            require(struct.unpack_from('<I', block, 24)[0] & 0xffff == 13, 'Unexpected APFS volume object type')
            require(block[:8] == checksum(block), 'Invalid APFS superblock checksum')
            require(struct.unpack_from('<Q', block, 264)[0] & 9 == 1, 'Seed volume must be unencrypted')
            require(struct.unpack_from('<Q', block, 216)[0] == 0, 'Seed volume must not contain snapshots')
            found.append((offset + at, block))
    identities = {block[240:256] for _, block in found}
    require(len(identities) == 1 and bytes(16) not in identities, 'Require exactly one nonzero APFS volume UUID')
    return found


def packed_uuid(path, tree):
    return str(uuid.UUID(bytes=volume_blocks(path, tree)[0][1][240:256])).upper()


def assign_uuid(raw, packed, tree, old, new):
    # Only called for this run's newly created, detached raw image. Every
    # checkpoint is checked before any write; original OS images are never edited.
    blocks = volume_blocks(packed, tree)
    require(packed_uuid(packed, tree) == old, 'Formatted volume identity differs')
    changes = []
    with raw.open('rb') as stream:
        for offset, before in blocks:
            stream.seek(offset)
            require(stream.read(PAGE) == before, 'Raw APFS checkpoint differs from packed seed')
            after = bytearray(before)
            after[240:256] = uuid.UUID(new).bytes
            after[:8] = checksum(after)
            changes.append((offset, bytes(after)))
    with raw.open('r+b') as stream:
        for offset, after in changes:
            stream.seek(offset)
            require(stream.write(after) == PAGE, 'Short APFS checkpoint write')
        stream.flush()
        os.fsync(stream.fileno())
    return dict(previous_uuid=old, requested_uuid=new, checkpoints=len(changes))


def expand(packed, raw, tree):
    logical, extents = regions(packed, tree)
    with raw.open('xb') as stream:
        stream.truncate(logical)
        for offset, data in extents:
            stream.seek(offset)
            require(stream.write(data) == len(data), 'Short seed expansion')


def preferences(mount):
    result = {}
    for name in ('com.apple.migration', 'com.apple.purplebuddy', 'com.apple.springboard', 'com.apple.UIKit'):
        path = mount / 'mobile/Library/Preferences' / (name + '.plist')
        require(not path.is_symlink(), 'Seed preference must not be a symlink')
        result[name] = plistlib.loads(path.read_bytes()) if path.is_file() else None
    return result


def verify_settings(settings, skip, source):
    sb, ui = settings['com.apple.springboard'], settings['com.apple.UIKit']
    require(sb and sb.get('BSUIVibrancyEffectViewDisabled') is True, 'Missing software renderer preference')
    require(ui and ui.get('UIStatusBarVisualProviderClassName') == '_UIStatusBarVisualProvider_Pad_ForcedNoCell'
            and ui.get('StatusBarHidesDate') is True, 'Missing reviewed UIKit preferences')
    if skip:
        migration, buddy = settings['com.apple.migration'], settings['com.apple.purplebuddy']
        require(migration and migration.get('LastSystemVersion') == target.BUILD and
                migration.get('DMUserDataDisposition') == {'SyntheticDidUpgrade': False, 'buildVersion': target.BUILD},
                'Wrong migration build')
        require(buddy and all(buddy.get(k) == v for k, v in dict(SetupDone=True, SetupFinishedAllSteps=True,
                SetupVersion=11, daemonizeNeedsToRun=False).items()), 'Setup skip preferences differ')
    else:
        for name in ('com.apple.migration', 'com.apple.purplebuddy'):
            require(settings[name] == source[name], 'Setup enabled must preserve original Setup preferences')
        require(not (settings['com.apple.purplebuddy'] or {}).get('SetupDone'), 'Setup is already complete')


def generate(tree, source, original_trust, library, work, output, request):
    require(sys.platform == 'darwin' and os.geteuid() == 0, 'Seed generation requires native macOS and root')
    require(not work.exists() and not output.exists(), 'Generation requires new work and output directories')
    prepare = legacy.load_script('prepare-picasso-ram-data')
    prepare.DATA_BYTES = request['data_size_gib'] * GIB
    work.mkdir(parents=True)
    config = work / 'seed-config.txt'
    shutil.copyfile(ROOT / 'configs/picasso_defconfig', config)
    prepare.prepare(tree, source, work / 'base', config)
    base = work / 'base'
    seed = json.loads((base / 'seed.json').read_text())
    raw, packed = base / 'data.cdr', base / 'data.dbrdisk'
    pack = runpy.run_path(str(tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'))['pack']
    identity = seed['volume_uuid']
    reassigned = None
    if request['data_uuid'] and request['data_uuid'] != identity:
        reassigned = assign_uuid(raw, packed, tree, identity, request['data_uuid'])
        identity = request['data_uuid']
        packed.rename(base / 'before-uuid.dbrdisk')
        pack(raw, packed)
        seed.update(prepare.inspect_packed(packed, raw=raw), volume_uuid=identity)
        write_json(base / 'seed.json', seed)
        config.write_text(prepare.configured_text(config.read_text(), identity), encoding='utf-8')
    require(packed_uuid(packed, tree) == identity, 'Packed base UUID differs')
    jobs = prepare.offline_jobs(legacy.board()['payload'])
    with prepare.mounted(raw, work / 'base-audit', readonly=True) as info:
        require(prepare.canonical_uuid(info['VolumeUUID']) == identity, 'Mounted base UUID differs')
        subprocess.run(['diskutil', 'verifyVolume', str(work / 'base-audit')], check=True)
        policy = prepare.verify_offline_policy(work / 'base-audit', jobs)
        prepare.verify_var(work / 'base-audit', json.loads((base / 'source-var-manifest.json').read_text()))
        source_preferences = preferences(work / 'base-audit')
    transform_tool = legacy.load_script('picasso-seed-skip-setup')
    setup = work / 'setup.dbr'
    if request['skip_setup']:
        # Load the module object so the existing board wrapper can configure it.
        import importlib.util
        spec = importlib.util.spec_from_file_location('generate_setup', tree / 'scripts/seed-data-skip-setup.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.BUILD, module.VERSION = target.BUILD, target.VERSION
        transform_tool.configure_seed(module, library, transform_tool.STATUSBAR_PROVIDER)
        (tree / 'build').mkdir(exist_ok=True)
        previous = sys.argv
        try:
            sys.argv = [str(spec.origin), str(packed), str(setup)]
            if '--no-auto-dim' in legacy.board()['payload']['skip_setup_args']:
                sys.argv.append('--no-auto-dim')
            module.main()
        finally:
            sys.argv = previous
    else:
        payload_raw = work / 'setup-enabled.cdr'
        expand(packed, payload_raw, tree)
        import types
        # Reuse the existing injection wrapper, applying only rendering settings.
        def merge_pref(mount, name, values):
            path = mount / 'mobile/Library/Preferences' / (name + '.plist')
            data = plistlib.loads(path.read_bytes()) if path.exists() else {}
            data.update(values)
            path.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
            os.chown(path, 501, 501)
            os.chmod(path, 0o600)
        module = types.SimpleNamespace(merge_pref=merge_pref)
        transform_tool.configure_seed(module, library, transform_tool.STATUSBAR_PROVIDER)
        with prepare.mounted(payload_raw, work / 'setup-enabled-mount'):
            module.merge_pref(work / 'setup-enabled-mount', 'com.apple.springboard', {})
        metadata = pack(payload_raw, setup)
        write_json(setup.with_suffix('.json'), dict(base_sha256=seed['seed_identity_sha256'],
                   output=metadata, preferences=['com.apple.springboard', 'com.apple.UIKit'],
                   no_auto_dim=False, build=target.BUILD, skip_setup=False))
    setup_info = prepare.inspect_packed(setup)
    require(packed_uuid(setup, tree) == identity, 'Setup transformation changed UUID')
    audited_raw = work / 'payload-audit.cdr'
    expand(setup, audited_raw, tree)
    require(prepare.inspect_packed(setup, raw=audited_raw) == setup_info, 'Payload expansion differs')
    checker = legacy.load_script('check-picasso-data-artifacts')
    with prepare.mounted(audited_raw, work / 'payload-audit', readonly=True) as info:
        require(prepare.canonical_uuid(info['VolumeUUID']) == identity, 'Mounted payload UUID differs')
        subprocess.run(['diskutil', 'verifyVolume', str(work / 'payload-audit')], check=True)
        payload_policy = prepare.verify_offline_policy(work / 'payload-audit', jobs)
        actual_preferences = preferences(work / 'payload-audit')
        verify_settings(actual_preferences, request['skip_setup'], source_preferences)
        if request['skip_setup']:
            payload_settings = checker.verify_setup_preferences(work / 'payload-audit', legacy.board()['payload'])
        else:
            injected = work / 'payload-audit/picasso/usr-lib/libpicasso-sb.dylib'
            require(prepare.metadata(injected) == dict(type='file', uid=0, gid=0, mode=0o644,
                    bytes=library.stat().st_size, sha256=legacy.digest(library)), 'Injected UI metadata differs')
            payload_settings = dict(preferences=list(actual_preferences),
                    injected_library=dict(path='picasso/usr-lib/libpicasso-sb.dylib', sha256=legacy.digest(injected)))
    require(payload_policy == policy == seed['offline_launch_policy'], 'Offline launch policy changed')
    bundle, ui = trust.make_bundle(original_trust.read_bytes(), library.read_bytes())
    transform = json.loads(setup.with_suffix('.json').read_text())
    audit = dict(scope='seed-only', kernel_audited=False, hardware_tested=False,
                 ui_trust=ui, packed=prepare.inspect_packed(packed), volume_uuid=identity,
                 payload_packed=setup_info, payload_seed_transformation=transform,
                 persistent=True, encrypted=False, offline_launch_policy=policy,
                 payload_offline_launch_policy=payload_policy, payload_setup_settings=payload_settings,
                 source_preferences=source_preferences, actual_preferences=actual_preferences,
                 uuid_assignment=reassigned, apfs_verify_volume=True)
    output.mkdir(parents=True)
    for name in ('data.dbrdisk', 'seed.json', 'source-var-manifest.json'):
        shutil.copyfile(base / name, output / name)
    shutil.copyfile(setup, output / 'setup.dbr')
    shutil.copyfile(setup.with_suffix('.json'), output / 'setup.json')
    shutil.copyfile(library, output / 'libpicasso-sb.dylib')
    shutil.copyfile(config, output / 'seed-config.txt')
    (output / 'ui-trust.bundle').write_bytes(bundle)
    write_json(output / 'data-audit.json', audit)
    compatible = request['data_size_gib'] == 20 and request['skip_setup']
    if compatible:
        write_json(output / 'manifest.json', dict(schema=legacy.SCHEMA, recipe_sha256=legacy.recipe(tree),
                   files={name: dict(bytes=(output / name).stat().st_size, sha256=legacy.digest(output / name)) for name in legacy.FILES}))
    manifest = dict(schema=SCHEMA, recipe_sha256=legacy.recipe(tree), generator_sha256=legacy.digest(Path(__file__)),
                    target=dict(product=target.PRODUCT, build=target.BUILD), parameters=request,
                    volume_uuid=identity, legacy_compatible=compatible,
                    producer=dict(workflow=WORKFLOW, revision=os.environ.get('GITHUB_SHA'), run_id=os.environ.get('GITHUB_RUN_ID')),
                    files={name: dict(bytes=(output / name).stat().st_size, sha256=legacy.digest(output / name))
                           for name in (*FILES, *(('manifest.json',) if compatible else ()))})
    write_json(output / 'seed-bundle.json', manifest)
    validate(output, tree)
    print(json.dumps(manifest, indent=2))


def validate(directory, tree):
    manifest_path = directory / 'seed-bundle.json'
    require(manifest_path.is_file() and not manifest_path.is_symlink() and manifest_path.stat().st_size <= 65536,
            'Invalid seed bundle manifest')
    manifest = json.loads(manifest_path.read_text())
    require(manifest['schema'] == SCHEMA and manifest['recipe_sha256'] == legacy.recipe(tree), 'Seed recipe differs')
    require(re.fullmatch('[0-9a-f]{64}', manifest['generator_sha256']) and
            manifest['producer']['workflow'] == WORKFLOW, 'Invalid seed producer')
    require(manifest['target'] == dict(product=target.PRODUCT, build=target.BUILD), 'Seed target differs')
    request = manifest['parameters']
    require(type(request['skip_setup']) is bool, 'Invalid Setup mode')
    require(request == options(str(request['data_size_gib']), request['data_uuid'],
                str(request['skip_setup']).lower(), str(request['retention_days'])), 'Invalid seed parameters')
    compatible = request['data_size_gib'] == 20 and request['skip_setup']
    require(manifest['legacy_compatible'] is compatible, 'Wrong legacy compatibility claim')
    names = (*FILES, *(('manifest.json',) if compatible else ()))
    require(set(manifest['files']) == set(names), 'Incomplete seed bundle')
    for name in names:
        path, entry = directory / name, manifest['files'][name]
        limit = 16 * 1024**2 if name.endswith('.json') else 4 * 1024**2
        require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= limit, 'Invalid seed member: ' + name)
        require(entry == dict(bytes=path.stat().st_size, sha256=legacy.digest(path)), 'Seed member hash differs: ' + name)
    prepare = legacy.load_script('prepare-picasso-ram-data')
    seed = json.loads((directory / 'seed.json').read_text())
    audit = json.loads((directory / 'data-audit.json').read_text())
    transform = json.loads((directory / 'setup.json').read_text())
    base = prepare.inspect_packed(directory / 'data.dbrdisk')
    payload = prepare.inspect_packed(directory / 'setup.dbr')
    identity = packed_uuid(directory / 'data.dbrdisk', tree)
    require(identity == packed_uuid(directory / 'setup.dbr', tree) == seed['volume_uuid'] == manifest['volume_uuid'], 'Seed UUID differs')
    require(not request['data_uuid'] or identity == request['data_uuid'], 'Requested UUID differs')
    require(base['logical_bytes'] == payload['logical_bytes'] == request['data_size_gib'] * GIB, 'Seed capacity differs')
    require(all(seed.get(k) == v for k, v in base.items()) and seed['schema'] == 'picasso-ram-data-v1', 'Base report differs')
    require(seed['source_sha256'] == target.OS_SHA256 and seed['source_launchd_sha256'] == target.LAUNCHD_SHA256,
            'Source OS differs')
    require(seed['ownership_preserved'] is True and seed['os_inputs_modified'] is False and seed['persistent'] is False,
            'Missing base ownership/source checks')
    require(seed['source_manifest_sha256'] == legacy.digest(directory / 'source-var-manifest.json') and
            seed['packer_sha256'] == legacy.digest(tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'), 'Source inventory or packer differs')
    require(transform['base_sha256'] == base['seed_identity_sha256'] and transform['build'] == target.BUILD,
            'Transformation base/build differs')
    require(transform['output']['packed'] is True, 'Payload must remain packed')
    for key, field in (('logical_bytes', 'bytes'), ('seed_identity_sha256', 'sha256'),
                       ('packed_bytes', 'encoded_bytes'), ('packed_sha256', 'encoded_sha256')):
        require(payload[key] == transform['output'][field], 'Transformation output differs: ' + key)
    require(audit['scope'] == 'seed-only' and audit['kernel_audited'] is False and audit['hardware_tested'] is False
            and audit['apfs_verify_volume'] is True and audit['encrypted'] is False, 'Invalid seed audit scope')
    require(audit['packed'] == base and audit['payload_packed'] == payload and audit['volume_uuid'] == identity
            and audit['payload_seed_transformation'] == transform, 'Audit inputs differ')
    ui = trust.verify_bundle((directory / 'ui-trust.bundle').read_bytes(), (directory / 'libpicasso-sb.dylib').read_bytes())
    require(audit['ui_trust'] == ui, 'UI trust differs')
    require(audit['payload_setup_settings']['injected_library'] == dict(path='picasso/usr-lib/libpicasso-sb.dylib',
            sha256=ui['library']['sha256']), 'Injected library audit differs')
    jobs = prepare.offline_jobs(legacy.board()['payload'])
    policy = dict(disabled_jobs=jobs, watchdogd_disabled='com.apple.watchdogd' in jobs, files=[])
    for name, blob in prepare.offline_policy_contents(jobs).items():
        policy['files'].append(dict(path=prepare.OFFLINE_POLICY_DIR + '/' + name, type='file', uid=0, gid=0,
            mode=0o644, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest()))
    require(seed['offline_launch_policy'] == audit['offline_launch_policy'] == audit['payload_offline_launch_policy'] == policy,
            'Offline policy differs')
    verify_settings(audit['actual_preferences'], request['skip_setup'], audit['source_preferences'])
    config = (directory / 'seed-config.txt').read_text()
    require(config == prepare.configured_text(config, identity), 'Seed configuration UUID differs')
    if compatible:
        legacy.validate(directory, tree)
    return manifest


def stage(directory, tree, work):
    manifest = validate(directory, tree)
    # Refuse replacement even when a caller accidentally selects an active build.
    config = work / '.config'
    require(config.is_file(), 'Run unixnu defconfig before staging seeds')
    destinations = [work / 'ram-data', work / 'generated-seed', work / 'libpicasso-sb.dylib', work / 'ui-trust.bundle']
    require(not any(p.exists() or p.is_symlink() for p in destinations), 'Seed destinations already exist; use a fresh build output')
    prepare = legacy.load_script('prepare-picasso-ram-data')
    configured = prepare.configured_text(config.read_text(), manifest['volume_uuid'])
    (work / 'ram-data').mkdir()
    for name in ('data.dbrdisk', 'seed.json', 'source-var-manifest.json'):
        shutil.copyfile(directory / name, work / 'ram-data' / name)
    (work / 'generated-seed').mkdir()
    for name in (*manifest['files'], 'seed-bundle.json'):
        shutil.copyfile(directory / name, work / 'generated-seed' / name)
    for name in ('libpicasso-sb.dylib', 'ui-trust.bundle'):
        shutil.copyfile(directory / name, work / name)
    config.write_text(configured, encoding='utf-8')
    result = dict(volume_uuid=manifest['volume_uuid'], parameters=manifest['parameters'],
                  validation_scope='seed-only; kernel and payload must be checked after compilation',
                  data_ramdisk=str(work / 'generated-seed/setup.dbr'), skip_setup=False,
                  trust_cache=str(work / 'ui-trust.bundle'))
    if manifest['legacy_compatible']:
        result.update(data_ramdisk=str(work / 'ram-data/data.dbrdisk'), skip_setup=True,
                      PICASSO_LINUX_INPUTS=str(work / 'generated-seed'))
    write_json(work / 'generated-seed/staged-inputs.json', result)
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('options', 'generate', 'validate', 'stage'))
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--original-trust', type=Path)
    parser.add_argument('--library', type=Path, default=ROOT / 'out/picasso/libpicasso-sb.dylib')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--request', type=Path)
    args = parser.parse_args()
    if args.command == 'options':
        request = options(os.environ.get('DATA_SIZE_GIB', '20'), os.environ.get('DATA_UUID', ''),
                          os.environ.get('SKIP_SETUP', 'true'), os.environ.get('RETENTION_DAYS', '90'))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_json(args.output, request)
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as stream:
                stream.write(f"retention_days={request['retention_days']}\n")
        print(json.dumps(request))
        return
    require(args.tree is not None, '--tree is required')
    tree, output = args.tree.resolve(), args.output.resolve()
    if args.command == 'generate':
        require(all((args.source, args.original_trust, args.work, args.request)), 'Missing generation inputs')
        request = json.loads(args.request.read_text())
        require(type(request['skip_setup']) is bool and request == options(str(request['data_size_gib']), request['data_uuid'],
                str(request['skip_setup']).lower(), str(request['retention_days'])), 'Invalid generation request')
        generate(tree, args.source.resolve(), args.original_trust.resolve(), args.library.resolve(),
                 args.work.resolve(), output, request)
    elif args.command == 'stage':
        require(args.work is not None, '--work is required')
        stage(output, tree, args.work.resolve())
    else:
        print(json.dumps(validate(output, tree), indent=2))


if __name__ == '__main__':
    main()
