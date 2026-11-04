#!/usr/bin/env python3
"""Reuse exact, audited iOS16 Compose inputs; build the current kernel on Linux."""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import tomllib

ROOT = Path(os.environ.get('UNIXNU_DEVICE', Path(__file__).resolve().parents[1])).resolve()
SCHEMA = 'picasso-ios16-linux-inputs-v1'
FILES = ('data.dbrdisk', 'seed.json', 'source-var-manifest.json', 'setup.dbr',
         'setup.json', 'libpicasso-sb.dylib', 'data-audit.json')


def load_script(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def board():
    return tomllib.loads((ROOT / 'board.toml').read_text(encoding='utf-8'))


def recipe(tree):
    sha = hashlib.sha256(SCHEMA.encode() + b'\0')
    for name in ('picasso_linux_inputs.py', 'picasso_target.py', 'prepare-picasso-ram-data.py',
                 'picasso-seed-skip-setup.py', 'picasso_ui_trust.py', 'build-picasso-sb-inject.py',
                 'check-picasso-data-artifacts.py'):
        sha.update(name.encode() + b'\0' + (ROOT / 'scripts' / name).read_bytes() + b'\0')
    for path in (ROOT / 'package/picasso-springboard/picasso-springboard-probe.m',
                 tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py',
                 tree / 'scripts/seed-data-skip-setup.py'):
        sha.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
    spec = board()['payload']
    settings = {key: spec.get(key) for key in ('skip_setup_tool', 'skip_setup_args',
                'offline_disabled_jobs', 'owned_trust_library', 'size_mib', 'adt')}
    sha.update(json.dumps(settings, sort_keys=True).encode())
    return sha.hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(directory, tree):
    path = directory / 'manifest.json'
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= 16 * 1024**2,
            'Invalid Compose input manifest')
    manifest = json.loads(path.read_text(encoding='utf-8'))
    require(manifest['schema'] == SCHEMA and manifest['recipe_sha256'] == recipe(tree),
            'iOS16 Compose input recipe differs; refresh Compose on this branch')
    require(set(manifest['files']) == set(FILES), 'Incomplete iOS16 Compose inputs')
    for name in FILES:
        path = directory / name
        limit = 4 * 1024**2 if name.endswith(('.dbr', '.dbrdisk', '.dylib')) else 16 * 1024**2
        entry = manifest['files'][name]
        require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= limit,
                'Invalid or unbounded Compose input: ' + name)
        require(path.stat().st_size == entry['bytes'] and digest(path) == entry['sha256'],
                'Compose input hash mismatch: ' + name)
    prepare = load_script('prepare-picasso-ram-data')
    trust = load_script('picasso_ui_trust')
    seed = json.loads((directory / 'seed.json').read_text())
    packed = prepare.inspect_packed(directory / 'data.dbrdisk')
    require(seed['schema'] == 'picasso-ram-data-v1' and
            all(seed.get(key) == value for key, value in packed.items()), 'Invalid base Data report')
    require(packed['logical_bytes'] == prepare.DATA_BYTES and packed['packed_bytes'] <= prepare.MAX_PACKED,
            'Wrong iOS16 Data capacity')
    require(seed['source_sha256'] == prepare.OS_SHA256 and seed['source_launchd_sha256'] == prepare.LAUNCHD_SHA256,
            'Data seed belongs to another OS')
    require(seed['persistent'] is False and seed['ownership_preserved'] is True and seed['os_inputs_modified'] is False,
            'Missing read-only/ownership audit')
    require(seed['packer_sha256'] == digest(tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py') and
            seed['source_manifest_sha256'] == digest(directory / 'source-var-manifest.json'), 'Stale seed inventory/packer')
    identity = prepare.canonical_uuid(seed['volume_uuid'])
    setup = prepare.inspect_packed(directory / 'setup.dbr')
    transform = json.loads((directory / 'setup.json').read_text())
    target = load_script('picasso_target')
    require(transform['base_sha256'] == packed['raw_sha256'] and transform['build'] == target.BUILD,
            'Setup seed has another base or iOS build')
    arguments = board()['payload']['skip_setup_args']
    require(transform['no_auto_dim'] is ('--no-auto-dim' in arguments), 'Setup dim policy differs')
    require(transform['output']['packed'] is True and setup['logical_bytes'] == packed['logical_bytes'],
            'Setup seed is not the same packed logical volume')
    for key, field in (('logical_bytes', 'bytes'), ('raw_sha256', 'sha256'),
                       ('packed_bytes', 'encoded_bytes'), ('packed_sha256', 'encoded_sha256')):
        require(setup[key] == transform['output'][field], 'Setup report hash/geometry mismatch: ' + key)
    audit = json.loads((directory / 'data-audit.json').read_text())
    library = trust.library_identity((directory / 'libpicasso-sb.dylib').read_bytes())
    require(audit['ui_trust']['library'] == library and audit['ui_trust']['original_modules_unchanged'] is True and
            audit['ui_trust']['owned_entries'] == 1 and
            audit['ui_trust']['original_bundle_sha256'] == trust.TRUST_BUNDLE_SHA256,
            'Exact signed UI library audit differs')
    require(audit['packed'] == packed and prepare.canonical_uuid(audit['volume_uuid']) == identity,
            'macOS audit has another base seed or Data UUID')
    require(audit['payload_packed'] == setup and audit['payload_seed_transformation'] == transform and
            audit['persistent'] is False, 'Actual macOS payload audit differs')
    require(audit['offline_launch_policy'] == seed['offline_launch_policy'] and
            audit['payload_offline_launch_policy'] == seed['offline_launch_policy'], 'Offline launch policy differs')
    jobs = prepare.offline_jobs(board()['payload'])
    expected_policy = dict(disabled_jobs=jobs, watchdogd_disabled='com.apple.watchdogd' in jobs, files=[])
    for name, blob in prepare.offline_policy_contents(jobs).items():
        expected_policy['files'].append(dict(path=prepare.OFFLINE_POLICY_DIR + '/' + name,
            type='file', uid=0, gid=0, mode=0o644, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest()))
    require(seed['offline_launch_policy'] == expected_policy, 'Offline policy does not match this board')
    settings = audit['payload_setup_settings']
    require(settings['injected_library'] == {'path': 'picasso/libpicasso-sb.dylib', 'sha256': library['sha256']},
            'The macOS Data seed does not contain the signed UI library')
    require(set(settings['preferences']) == {'com.apple.migration', 'com.apple.purplebuddy',
            'com.apple.springboard', 'com.apple.UIKit'}, 'Missing audited iOS16 Setup preferences')
    return dict(manifest=manifest, seed=seed, packed=packed, setup=setup, audit=audit, volume_uuid=identity)


def export(tree, work, output):
    require(os.uname().sysname == 'Darwin', 'Input export requires native macOS Compose/APFS checks')
    checker = load_script('check-picasso-data-artifacts')
    audit_path = work / 'logs/linux-input-audit.json'
    audit_path.parent.mkdir(parents=True, exist_ok=True)
    audit = checker.check(work, work / 'tree', audit_path)
    seed = json.loads((work / 'ram-data/seed.json').read_text())
    payload = json.loads((work / 'payload/payload.json').read_text())
    preparation = load_script('prepare-picasso-ram-data')
    setup, _, _ = checker.payload_seed(work, board()['payload'], payload, seed, preparation.inspect_packed)
    output.mkdir(parents=True, exist_ok=False)
    sources = {'data.dbrdisk': work / 'ram-data/data.dbrdisk', 'seed.json': work / 'ram-data/seed.json',
               'source-var-manifest.json': work / 'ram-data/source-var-manifest.json', 'setup.dbr': setup,
               'setup.json': setup.with_suffix('.json'), 'libpicasso-sb.dylib': work / 'libpicasso-sb.dylib',
               'data-audit.json': audit_path}
    for name, source in sources.items():
        shutil.copyfile(source, output / name)
    manifest = dict(schema=SCHEMA, recipe_sha256=recipe(tree),
                    files={name: {'bytes': (output / name).stat().st_size, 'sha256': digest(output / name)} for name in FILES})
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    validate(output, tree)
    print('PASS: exported audited iOS16 base/Setup seeds and exact signed status bar library')


def extract(archive, output):
    import zipfile
    selected = {}
    with zipfile.ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            parts = PurePosixPath(entry.filename).parts
            if len(parts) < 2 or parts[-2] != 'linux-inputs' or parts[-1] not in (*FILES, 'manifest.json'):
                continue
            name = parts[-1]
            limit = 4 * 1024**2 if name.endswith(('.dbr', '.dbrdisk', '.dylib')) else 16 * 1024**2
            require(name not in selected and not entry.is_dir() and
                    (entry.external_attr >> 16 & 0o170000) in (0, 0o100000) and 0 < entry.file_size <= limit,
                    'Duplicate, unsafe or unbounded iOS16 artifact member')
            selected[name] = entry
        require(set(selected) == {*FILES, 'manifest.json'}, 'Compose artifact has no reusable iOS16 input bundle')
        output.mkdir(exist_ok=False)
        for name, entry in selected.items():
            with bundle.open(entry) as source, (output / name).open('xb') as target:
                shutil.copyfileobj(source, target)


def api(repository, path):
    result = subprocess.run(['gh', 'api', f'repos/{repository}/{path}'], capture_output=True, check=True)
    return json.loads(result.stdout)


def acquire(repository, branch, tree, output):
    from urllib.parse import quote
    script_sha = digest(ROOT / 'scripts/picasso_linux_inputs.py')
    seen = set()
    for producer in dict.fromkeys((branch, 'main', None)):
        query = '&branch=' + quote(producer, safe='') if producer else ''
        runs = api(repository, 'actions/workflows/compose-bootimg.yml/runs?status=success&per_page=100' + query)['workflow_runs']
        for run in runs:
            if run['head_sha'] in seen:
                continue
            if run['conclusion'] != 'success' or run['event'] != 'workflow_dispatch' or run['path'] != '.github/workflows/compose-bootimg.yml':
                continue
            try:
                source = api(repository, 'contents/scripts/picasso_linux_inputs.py?ref=' + run['head_sha'])
            except subprocess.CalledProcessError as error:
                if b'(HTTP 404)' not in (error.stderr or b''):
                    raise
                seen.add(run['head_sha'])
                continue  # Older Compose revisions do not export reusable iOS16 inputs.
            if hashlib.sha256(base64.b64decode(source['content'])).hexdigest() != script_sha:
                seen.add(run['head_sha'])
                continue
            artifacts = api(repository, f'actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
            for artifact in artifacts:
                if artifact['expired'] or not artifact['name'].startswith('picasso-bootimg-'):
                    continue
                checksum = artifact.get('digest', '')
                require(checksum.startswith('sha256:') and len(checksum) == 71 and
                        0 < artifact['size_in_bytes'] <= 512 * 1024**2, 'Missing bounded artifact SHA256 identity')
                output.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.TemporaryDirectory(prefix='ios16-compose-', dir=output.parent) as temporary:
                    archive = Path(temporary) / 'compose.zip'
                    with archive.open('xb') as stream:
                        subprocess.run(['gh', 'api', f'repos/{repository}/actions/artifacts/{artifact["id"]}/zip'],
                                       stdout=stream, check=True)
                    require(archive.stat().st_size == artifact['size_in_bytes'] and digest(archive) == checksum[7:],
                            'Compose archive hash/size mismatch')
                    staged = Path(temporary) / 'inputs'
                    extract(archive, staged)
                    try:
                        validate(staged, tree)
                    except ValueError:
                        continue  # A matching exporter can serve another iOS16 seed recipe.
                    shutil.copytree(staged, output)
                provenance = dict(schema=SCHEMA, repository=repository, recipe_sha256=recipe(tree),
                    producer_run_id=run['id'], producer_revision=run['head_sha'], producer_branch=run['head_branch'],
                    producer_workflow=run['path'], artifact_id=artifact['id'], artifact_sha256=checksum[7:])
                (output / 'reuse.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
                return
    raise ValueError('No compatible iOS16 Compose input bundle is retained. Run macOS Compose once on feat/ios16-gpu, then run Linux.')


def reuse(tree, output, work, repository, branch):
    if not output.exists():
        acquire(repository, branch, tree, output)
    proof = json.loads((output / 'reuse.json').read_text())
    require(proof['schema'] == SCHEMA and proof['repository'] == repository and
            proof['recipe_sha256'] == recipe(tree) and proof['producer_workflow'] == '.github/workflows/compose-bootimg.yml',
            'Cached Compose input provenance differs')
    require(all(type(proof[key]) is int and proof[key] > 0 for key in ('producer_run_id', 'artifact_id')) and
            re.fullmatch('[0-9a-f]{40}', proof['producer_revision']) and
            re.fullmatch('[0-9a-f]{64}', proof['artifact_sha256']) and bool(proof['producer_branch']),
            'Incomplete Compose producer identity')
    inputs = validate(output, tree)
    preparation = load_script('prepare-picasso-ram-data')
    (work / 'ram-data').mkdir(parents=True, exist_ok=True)
    for name in ('data.dbrdisk', 'seed.json', 'source-var-manifest.json'):
        shutil.copyfile(output / name, work / 'ram-data' / name)
    shutil.copyfile(output / 'libpicasso-sb.dylib', work / 'libpicasso-sb.dylib')
    config = work / '.config'
    config.write_text(preparation.configured_text(config.read_text(), inputs['volume_uuid']), encoding='utf-8', newline='\n')
    print(json.dumps(dict(volume_uuid=inputs['volume_uuid'], producer_run_id=proof['producer_run_id'],
                         producer_branch=proof['producer_branch'], recipe_sha256=recipe(tree)), indent=2))


def stage_setup(base, output, arguments, tree):
    directory = os.environ.get('PICASSO_LINUX_INPUTS')
    require(directory, 'Linux --skip-setup requires PICASSO_LINUX_INPUTS from an audited Compose run')
    inputs = validate(Path(directory), tree)
    require(arguments == board()['payload']['skip_setup_args'], 'Linux Setup arguments differ from the audited recipe')
    require(digest(base) == inputs['packed']['packed_sha256'], 'Linux Setup base seed differs')
    source = ROOT / board()['payload']['owned_trust_library']
    require(digest(source) == digest(Path(directory) / 'libpicasso-sb.dylib'), 'Linux UI library differs from the seed')
    shutil.copyfile(Path(directory) / 'setup.dbr', output)
    shutil.copyfile(Path(directory) / 'setup.json', output.with_suffix('.json'))
    print('PASS: staged the exact audited macOS iOS16 Setup seed without mounting APFS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('key', 'export', 'reuse'))
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=ROOT / 'out/picasso')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/picasso/linux-inputs')
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY', 'YinCQL/UniXNU-picasso'))
    parser.add_argument('--branch', default=os.environ.get('GITHUB_REF_NAME', 'feat/ios16-gpu'))
    args = parser.parse_args()
    if args.command == 'key':
        print(recipe(args.tree.resolve()))
    elif args.command == 'export':
        export(args.tree.resolve(), args.work.resolve(), args.output.resolve())
    else:
        reuse(args.tree.resolve(), args.output.resolve(), args.work.resolve(), args.repository, args.branch)


if __name__ == '__main__':
    main()
