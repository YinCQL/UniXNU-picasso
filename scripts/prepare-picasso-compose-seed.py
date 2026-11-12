#!/usr/bin/env python3
"""Import an explicitly selected, validated seed artifact for Compose."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import picasso_linux_inputs as inputs


def select(repository, artifact_id):
    if not re.fullmatch('[1-9][0-9]{0,19}', artifact_id) or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid explicit seed artifact identity')
    item = inputs.api(repository, 'actions/artifacts/' + artifact_id)
    run = inputs.api(repository, 'actions/runs/' + str(item['workflow_run']['id']))
    workflow = run['path']
    producer = item['workflow_run']
    if (str(item['id']) != artifact_id or item['expired'] or run['id'] != producer['id'] or
            producer['head_sha'] != run['head_sha'] or producer['head_branch'] != run['head_branch'] or
            producer['repository_id'] != run['repository']['id'] or
            producer['head_repository_id'] != run['repository']['id'] or
            run['head_repository']['id'] != run['repository']['id'] or
            not re.fullmatch('[0-9a-f]{40}', run['head_sha']) or
            run['status'] != 'completed' or run['conclusion'] != 'success' or
            run['event'] != 'workflow_dispatch' or run['repository']['full_name'] != repository or
            workflow not in ('.github/workflows/compose-bootimg.yml', '.github/workflows/generate-data-seed.yml')):
        raise ValueError('Seed must come from a successful, retained repository seed/Compose run')
    prefix = 'picasso-data-seed-' if workflow.endswith('/compose-bootimg.yml') else 'picasso-generated-data-seed-'
    if item['name'] != prefix + str(run['run_number']):
        raise ValueError('Artifact is not a dedicated Data seed')
    checksum = item.get('digest', '')
    if not re.fullmatch('sha256:[0-9a-f]{64}', checksum) or not 0 < item['size_in_bytes'] <= 32 * 1024**2:
        raise ValueError('Missing bounded seed archive size/hash')
    source = inputs.api(repository, 'contents/scripts/picasso_linux_inputs.py?ref=' + run['head_sha'])
    if (source['encoding'] != 'base64' or
            hashlib.sha256(base64.b64decode(source['content'])).hexdigest() !=
            inputs.digest(inputs.ROOT / 'scripts/picasso_linux_inputs.py')):
        raise ValueError('Seed producer uses a different input validator')
    return item, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    artifact_id = os.environ['PICASSO_SEED_ARTIFACT_ID']
    repository = os.environ['GITHUB_REPOSITORY']
    item, run = select(repository, artifact_id)
    checksum, workflow = item['digest'], run['path']
    tree, work, output = args.tree.resolve(), args.work.resolve(), args.output.resolve()
    if output.exists():
        raise ValueError('Seed output already exists')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='explicit-seed-', dir=output.parent) as temporary:
        archive = Path(temporary) / 'seed.zip'
        with archive.open('xb') as stream:
            subprocess.run(['gh', 'api', f'repos/{repository}/actions/artifacts/{artifact_id}/zip'], stdout=stream, check=True)
        if archive.stat().st_size != item['size_in_bytes'] or inputs.digest(archive) != checksum[7:]:
            raise ValueError('Seed archive differs from GitHub identity')
        staged = Path(temporary) / 'inputs'
        inputs.extract(archive, staged)
        checked = inputs.validate(staged, tree)
        shutil.copytree(staged, output)
    preparation = inputs.load_script('prepare-picasso-ram-data')
    configured = preparation.configured_text((work / '.config').read_text(), checked['volume_uuid'])
    (work / 'ram-data').mkdir(parents=True, exist_ok=True)
    for name in ('data.dbrdisk', 'seed.json', 'source-var-manifest.json'):
        shutil.copyfile(output / name, work / 'ram-data' / name)
    shutil.copyfile(output / 'libpicasso-sb.dylib', work / 'libpicasso-sb.dylib')
    (work / '.config').write_text(configured, encoding='utf-8', newline='\n')
    proof = dict(repository=repository, artifact_id=int(artifact_id), artifact_sha256=checksum[7:],
        producer_workflow=workflow, producer_run_id=run['id'], producer_revision=run['head_sha'],
        volume_uuid=checked['volume_uuid'], recipe_sha256=inputs.recipe(tree), explicit_selection=True)
    (output / 'imported-seed.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof))


if __name__ == '__main__':
    main()
