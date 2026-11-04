#!/usr/bin/env python3
"""Reuse an audited macOS APFS seed for Linux kernel/payload builds.

Only the packed Data seed and its reports are imported from a successful Compose
run. The current kernel, configuration and boot image are built independently.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import subprocess
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = ('data.dbrdisk', 'seed.json', 'source-var-manifest.json')
SCHEMA = 'picasso-linux-data-reuse-v1'


def prepare_module():
    spec = importlib.util.spec_from_file_location('picasso_data', ROOT / 'scripts/prepare-picasso-ram-data.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def cache_key(tree):
    sha = hashlib.sha256(SCHEMA.encode() + b'\0')
    for path in (ROOT / 'scripts/prepare-picasso-ram-data.py',
                 tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'):
        sha.update(path.read_bytes() + b'\0')
    return sha.hexdigest()


def validate_seed(directory, tree):
    module = prepare_module()
    seed = json.loads((directory / 'seed.json').read_text(encoding='utf-8'))
    packed = module.inspect_packed(directory / 'data.dbrdisk')
    if seed.get('schema') != 'picasso-ram-data-v1':
        raise ValueError('Unknown Data seed report')
    for name, value in packed.items():
        if seed.get(name) != value:
            raise ValueError(f'Data seed report mismatch: {name}')
    if packed['logical_bytes'] != module.DATA_BYTES or packed['packed_bytes'] > module.MAX_PACKED:
        raise ValueError('Data seed geometry exceeds the current memory/payload budget')
    if (seed.get('source_sha256') != module.OS_SHA256 or
            seed.get('source_launchd_sha256') != module.LAUNCHD_SHA256):
        raise ValueError('Data seed is from another original OS')
    if (seed.get('persistent') is not False or seed.get('ownership_preserved') is not True or
            seed.get('os_inputs_modified') is not False):
        raise ValueError('Data seed lacks the original read-only/ownership audit')
    if seed.get('packer_sha256') != digest(tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'):
        raise ValueError('Data seed packer differs from the current pinned source')
    if digest(directory / 'source-var-manifest.json') != seed.get('source_manifest_sha256'):
        raise ValueError('Data seed source inventory hash mismatch')
    preferences = {'SBDisableAutoDim': True}
    blob = plistlib.dumps(preferences, fmt=plistlib.FMT_BINARY, sort_keys=True)
    expected = [dict(path='mobile/Library/Preferences/com.apple.springboard.plist',
                     values=preferences, type='file', uid=501, gid=501, mode=0o600,
                     bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())]
    if seed.get('generated_preferences') != expected:
        raise ValueError('Data seed auto-dim preference audit differs')
    module.canonical_uuid(seed['volume_uuid'])
    return seed


def extract_seed(archive, output):
    """Extract only three bounded regular members, never ZIP paths or links."""
    with zipfile.ZipFile(archive) as bundle:
        selected = {}
        for info in bundle.infolist():
            parts = PurePosixPath(info.filename).parts
            if len(parts) < 2 or parts[-2] != 'ram-data' or parts[-1] not in FILES:
                continue
            name = parts[-1]
            mode = info.external_attr >> 16
            if name in selected or info.is_dir() or (mode & 0o170000) not in (0, 0o100000):
                raise ValueError('Duplicate or non-regular Data artifact member')
            limit = 4 * 1024**2 if name == 'data.dbrdisk' else 16 * 1024**2
            if not 0 < info.file_size <= limit:
                raise ValueError(f'Data artifact member exceeds its bound: {name}')
            selected[name] = info
        if set(selected) != set(FILES):
            raise ValueError('Compose artifact lacks the packed Data seed or source reports')
        output.mkdir(parents=True, exist_ok=False)
        for name, info in selected.items():
            with bundle.open(info) as source, (output / name).open('xb') as target:
                shutil.copyfileobj(source, target)


def api(repository, path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/{repository}/{path}']))


def acquire_seed(repository, branch, tree, output):
    script_sha = digest(ROOT / 'scripts/prepare-picasso-ram-data.py')
    # Prefer this branch, then main; only identical seed recipes are compatible.
    for producer_branch in dict.fromkeys((branch, 'main')):
        from urllib.parse import quote
        runs = api(repository, 'actions/workflows/compose-bootimg.yml/runs'
                   f'?status=success&branch={quote(producer_branch, safe="")}&per_page=100')['workflow_runs']
        for run in runs:
            if (run['conclusion'] != 'success' or run['event'] != 'workflow_dispatch' or
                    run['head_branch'] != producer_branch or
                    run['path'] != '.github/workflows/compose-bootimg.yml'):
                continue
            artifacts = api(repository, f'actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
            artifacts = [a for a in artifacts if not a['expired'] and a['name'].startswith('picasso-bootimg-')]
            if not artifacts:
                continue
            source = api(repository, 'contents/scripts/prepare-picasso-ram-data.py?ref=' + run['head_sha'])
            if hashlib.sha256(base64.b64decode(source['content'])).hexdigest() != script_sha:
                continue
            artifact, = artifacts
            checksum = artifact.get('digest', '')
            if (not checksum.startswith('sha256:') or len(checksum) != 71 or
                    not 0 < artifact['size_in_bytes'] <= 512 * 1024**2):
                raise ValueError('Compose artifact lacks a bounded SHA256 identity')
            download = output.parent / ('data-seed-download-' + uuid.uuid4().hex)
            download.mkdir(parents=True, exist_ok=False)
            archive = download / 'compose.zip'
            # gh follows the artifact redirect without exposing credentials or a signed URL.
            with archive.open('xb') as stream:
                subprocess.run(['gh', 'api', f'repos/{repository}/actions/artifacts/{artifact["id"]}/zip'],
                               stdout=stream, check=True)
            if archive.stat().st_size != artifact['size_in_bytes'] or digest(archive) != checksum[7:]:
                raise ValueError('Compose artifact archive identity mismatch')
            staged = download / 'seed'
            extract_seed(archive, staged)
            validate_seed(staged, tree)
            output.mkdir(parents=True, exist_ok=False)
            for name in FILES:
                shutil.copyfile(staged / name, output / name)
            provenance = dict(schema=SCHEMA, cache_key=cache_key(tree), repository=repository,
                              producer_run_id=run['id'], producer_revision=run['head_sha'],
                              producer_branch=producer_branch, producer_workflow=run['path'],
                              artifact_id=artifact['id'], artifact_sha256=checksum[7:],
                              seed_recipe_sha256=script_sha)
            (output / 'reuse.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
            return
    raise ValueError('No retained successful Compose artifact has a compatible audited Data seed. '
                     'Run Compose Boot Image once to refresh the seed, then run the Linux workflow.')


def reuse(tree, output, config, repository, branch):
    if not output.exists():
        acquire_seed(repository, branch, tree, output)
    provenance = json.loads((output / 'reuse.json').read_text(encoding='utf-8'))
    if (provenance.get('schema') != SCHEMA or provenance.get('cache_key') != cache_key(tree) or
            provenance.get('repository') != repository or
            provenance.get('producer_workflow') != '.github/workflows/compose-bootimg.yml' or
            provenance.get('seed_recipe_sha256') != digest(ROOT / 'scripts/prepare-picasso-ram-data.py')):
        raise ValueError('Cached Data seed provenance differs from the current recipe')
    seed = validate_seed(output, tree)
    module = prepare_module()
    original = config.read_text(encoding='utf-8')
    selected = module.configured_text(original, seed['volume_uuid'])
    # Select the seed UUID before building the current kernel, never use an old .config.
    config.write_text(selected, encoding='utf-8', newline='\n')
    print(json.dumps(dict(volume_uuid=seed['volume_uuid'], packed_sha256=seed['packed_sha256'],
                          producer_run_id=provenance['producer_run_id'],
                          current_config_sha256=digest(config), persistent_seed=False), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY', 'YinCQL/UniXNU-picasso'))
    parser.add_argument('--branch', default=os.environ.get('GITHUB_REF_NAME', 'test/pr17'))
    parser.add_argument('--cache-key', action='store_true')
    args = parser.parse_args()
    if args.cache_key:
        print(cache_key(args.tree.resolve()))
        return
    if args.output is None or args.config is None:
        parser.error('--output and --config are required when reusing a seed')
    reuse(args.tree.resolve(), args.output.resolve(), args.config.resolve(), args.repository, args.branch)


if __name__ == '__main__':
    main()
