#!/usr/bin/env python3
"""Restore the exact retained Data seed when disposable Actions copies expire."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import urllib.request
import picasso_linux_inputs as inputs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    tree, work, output = a.tree.resolve(), a.work.resolve(), a.output.resolve()
    pin = json.loads((inputs.ROOT / 'configs/retained-data-seed.json').read_text())
    if pin['schema'] != 'picasso-retained-seed-v1' or pin['recipe_sha256'] != inputs.recipe(tree):
        raise ValueError('Retained seed recipe differs; never create a replacement implicitly')
    url = 'https://github.com/' + pin['repository'] + '/releases/download/' + pin['tag'] + '/' + pin['asset']
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='retained-seed-', dir=output.parent) as temporary:
        archive = Path(temporary) / 'seed.zip'
        with urllib.request.urlopen(url, timeout=60) as source, archive.open('xb') as dest:
            data = source.read(pin['bytes'] + 1)
            dest.write(data)
        if archive.stat().st_size != pin['bytes'] or inputs.digest(archive) != pin['sha256']:
            raise ValueError('Retained seed release hash/size differs')
        staged = Path(temporary) / 'inputs'
        inputs.extract(archive, staged)
        checked = inputs.validate(staged, tree)
        if checked['volume_uuid'] != pin['volume_uuid']:
            raise ValueError('Retained Data UUID differs')
        shutil.copytree(staged, output)
    preparation = inputs.load_script('prepare-picasso-ram-data')
    (work / 'ram-data').mkdir(parents=True, exist_ok=True)
    for name in ('data.dbrdisk', 'seed.json', 'source-var-manifest.json'):
        shutil.copyfile(output / name, work / 'ram-data' / name)
    shutil.copyfile(output / 'libpicasso-sb.dylib', work / 'libpicasso-sb.dylib')
    config = work / '.config'
    config.write_text(preparation.configured_text(config.read_text(), checked['volume_uuid']), encoding='utf-8', newline='\n')
    proof = dict(pin, source='retained-release', new_seed_created=False)
    (output / 'imported-seed.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof))


if __name__ == '__main__':
    main()
