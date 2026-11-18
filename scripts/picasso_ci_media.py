#!/usr/bin/env python3
"""Transfer signed media from a successful Compose of the exact device revision."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import zipfile
import picasso_linux_inputs as inputs
import picasso_media_payload as media

ROOT = inputs.ROOT
SCHEMA = 'picasso-signed-media-inputs-v1'
FILES = ('media.addon', 'media-trust.bundle', 'build-report.json', 'audio-report.json',
         'bluetoothd-stock', 'btaudio-stock', 'manifest.json')
PAYLOAD_MIB = media.PAYLOAD_MIB
kernel_args = media.kernel_args
addon = media.addon


def source_files():
    names = subprocess.check_output(['git', '-c', 'safe.directory=' + str(ROOT), '-C', str(ROOT), 'ls-files'], text=True).splitlines()
    return {n: inputs.digest(ROOT / n) for n in names if (ROOT / n).is_file()}


def checked(directory, tree):
    manifest = json.loads((directory / 'manifest.json').read_text())
    if (manifest['schema'] != SCHEMA or manifest['recipe_sha256'] != inputs.recipe(tree) or
            manifest['sources'] != source_files() or set(manifest['files']) != set(FILES) - {'manifest.json'}):
        raise ValueError('Signed media source/seed recipe differs')
    for name, pin in manifest['files'].items():
        path = directory / name
        if (path.is_symlink() or not path.is_file() or not 0 < pin['bytes'] <= 64 * 1024**2 or
                path.stat().st_size != pin['bytes'] or inputs.digest(path) != pin['sha256']):
            raise ValueError('Signed media file hash/size differs: ' + name)
    return manifest


def export(work, tree, output):
    media.verify((work / 'media/media-trust.bundle').read_bytes(), work)
    from picasso_bluetooth_payload import paths
    _, stock = paths(work)
    btaudio = Path(os.environ.get('PICASSO_BT_AUDIO_STOCK', work / 'bluetooth-input/BTAudioHALPlugin.driver'))
    sources = {'media.addon': media.addon(work), 'media-trust.bundle': work / 'media/media-trust.bundle',
               'build-report.json': work / 'media/build-report.json', 'audio-report.json': work / 'audio-hal/build-report.json',
               'bluetoothd-stock': stock, 'btaudio-stock': btaudio / 'BTAudioHALPlugin'}
    output.mkdir(parents=True, exist_ok=False)
    for name, source in sources.items():
        shutil.copyfile(source, output / name)
    manifest = dict(schema=SCHEMA, recipe_sha256=inputs.recipe(tree), sources=source_files(),
                    producer_revision=os.environ['GITHUB_SHA'], producer_run_id=int(os.environ['GITHUB_RUN_ID']),
                    files={n: dict(bytes=p.stat().st_size, sha256=inputs.digest(p)) for n, p in sources.items()})
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    checked(output, tree)


def acquire(work, tree, run_id):
    repository = os.environ['GITHUB_REPOSITORY']
    run = inputs.api(repository, 'actions/runs/' + run_id)
    revision = os.environ['GITHUB_SHA']
    if (run['conclusion'] != 'success' or run['status'] != 'completed' or
            run['path'] != '.github/workflows/compose-bootimg.yml' or run['event'] != 'workflow_dispatch' or
            run['head_sha'] != revision or run['head_branch'] != os.environ['GITHUB_REF_NAME'] or
            run['repository']['full_name'] != repository or run['head_repository']['id'] != run['repository']['id']):
        raise ValueError('Signed media requires successful Compose of this exact branch revision')
    artifacts = inputs.api(repository, 'actions/runs/' + run_id + '/artifacts?per_page=100')['artifacts']
    selected = [a for a in artifacts if a['name'] == 'picasso-signed-media-' + str(run['run_number']) and not a['expired']]
    seeds = [a for a in artifacts if a['name'] == 'picasso-data-seed-' + str(run['run_number']) and not a['expired']]
    if len(selected) != 1 or len(seeds) != 1:
        raise ValueError('Compose has no unique retained signed media and Data inputs')
    output = work / 'signed-media-inputs'
    with tempfile.TemporaryDirectory(prefix='compose-media-', dir=work) as temp:
        for item, is_seed in ((seeds[0], True), (selected[0], False)):
            checksum = item.get('digest', '')
            if not re.fullmatch('sha256:[0-9a-f]{64}', checksum) or not 0 < item['size_in_bytes'] <= 128 * 1024**2:
                raise ValueError('Compose artifact hash/size missing')
            archive = Path(temp) / ('seed.zip' if is_seed else 'media.zip')
            with archive.open('xb') as stream:
                subprocess.run(['gh', 'api', f'repos/{repository}/actions/artifacts/{item["id"]}/zip'], stdout=stream, check=True)
            if archive.stat().st_size != item['size_in_bytes'] or inputs.digest(archive) != checksum[7:]:
                raise ValueError('Compose archive hash/size differs')
            if is_seed:
                destination = work / 'linux-inputs'
                inputs.extract(archive, destination)
                inputs.validate(destination, tree)
                proof = dict(schema=inputs.SCHEMA, repository=repository, recipe_sha256=inputs.recipe(tree),
                             producer_run_id=run['id'], producer_revision=revision, producer_branch=run['head_branch'],
                             producer_workflow=run['path'], artifact_id=item['id'], artifact_sha256=checksum[7:])
                (destination / 'reuse.json').write_text(json.dumps(proof, indent=2) + '\n')
                inputs.reuse(tree, destination, work, repository, run['head_branch'])
            else:
                output.mkdir()
                with zipfile.ZipFile(archive) as z:
                    if sorted(z.namelist()) != sorted(FILES):
                        raise ValueError('Signed media archive inventory differs')
                    for entry in z.infolist():
                        if (entry.is_dir() or (entry.external_attr >> 16 & 0o170000) not in (0, 0o100000) or
                                not 0 < entry.file_size <= 64 * 1024**2):
                            raise ValueError('Unsafe/unbounded signed media archive entry')
                        with z.open(entry) as source, (output / entry.filename).open('xb') as dest:
                            shutil.copyfileobj(source, dest)
                manifest = checked(output, tree)
                if manifest['producer_revision'] != revision or manifest['producer_run_id'] != run['id']:
                    raise ValueError('Signed media producer manifest differs')
    for name in ('media.addon', 'media-trust.bundle', 'build-report.json'):
        (work / 'media').mkdir(exist_ok=True)
        shutil.copyfile(output / name, work / 'media' / name)
    # Stage the same signed HAL files for the unchanged full media/Data checker.
    from picasso_audio_payload import load
    hal = load(ROOT / 'scripts/build-picasso-audio-hal.py')
    audio_files = [f for f in media.unpack(addon(work).read_bytes()) if f[0] in hal.AUDIO_FILES]
    (work / 'audio-hal').mkdir(exist_ok=True)
    for name, _, blob in audio_files:
        path = work / 'audio-hal' / name.split('/', 1)[1]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    (work / 'audio-hal/audio.addon').write_bytes(hal.pack(audio_files))
    shutil.copyfile(output / 'audio-report.json', work / 'audio-hal/build-report.json')
    print(json.dumps(dict(passed=True, producer_run_id=run['id'], producer_revision=revision,
                         media_artifact_id=selected[0]['id'], firmware_source='repository V13.0.5.0.SGICNXM')))


def trust_module(hashes, prefix, metadata=None):
    hashes = sorted(hashes)
    module = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL, prefix + ':'.join(h.hex() for h in hashes)).bytes, len(hashes))
    return module + b''.join(h + (metadata[h] if metadata else bytes([2, 0, 0, 0])) for h in hashes)


def verify(blob, work):
    tree = Path(os.environ['UNIXNU_TREE'])
    checked(work / 'signed-media-inputs', tree)
    r = json.loads((work / 'media/build-report.json').read_text())
    raw = addon(work).read_bytes()
    entries = media.unpack(raw)
    if media.pack(entries) != raw or not r['passed'] or media.digest(raw) != r['addon_sha256'] or media.digest(blob) != r['trust_sha256']:
        raise ValueError('Combined signed media differs from Compose audit')
    files = {n: data for n, _, data in entries}
    if r['files'] != {n: dict(mode=m, bytes=len(b), sha256=media.digest(b)) for n, m, b in entries}:
        raise ValueError('Combined media inventory differs')
    from picasso_firmware import manifest, verify as firmware_verify
    for name, pin in manifest('bluetooth')['files'].items():
        firmware_verify(files['bluetooth-firmware/' + name], pin, name)
    from picasso_ui_trust import make_bundle
    original = (work / 'original-static-system.bundle').read_bytes()
    _, ui = make_bundle(original, (work / 'libpicasso-sb.dylib').read_bytes())
    runtime, _ = make_bundle(original, files['ui/libpicasso-sb.dylib'])
    modules = media.modules(runtime)
    from picasso_audio_payload import load
    hal = load(ROOT / 'scripts/build-picasso-audio-hal.py')
    from picasso_audio_route import verify as route_verify
    audio_identity = hal.bundle_identity(files['audio/PicassoAudio.driver/PicassoAudio'])
    route = route_verify(work / 'audio-hal')
    modules.append(trust_module([bytes.fromhex(audio_identity['cdhash']), bytes.fromhex(route['identity']['cdhash'])], 'picasso:audio:'))
    gpu = {n: media.identity(files[n]) for n, mode in media.GPU_FILES.items() if mode == 0o755}
    if gpu != r['gpu_identities']:
        raise ValueError('GPU signed library identity differs')
    modules.append(trust_module([bytes.fromhex(i['cdhash']) for i in gpu.values()], 'picasso:gpu:'))
    from picasso_bluetooth_payload import identity, blobs
    stock = (work / 'signed-media-inputs/bluetoothd-stock').read_bytes()
    bt = {}
    for path in ('bluetooth-bin/bluetoothd', 'bluetooth-bin/BlueTool', 'ui/PicassoBTType0.dylib', 'ui/PicassoBTAudio.dylib', 'audio/BTAudioHALPlugin.driver/BTAudioHALPlugin'):
        name = path.split('/')[-1]
        original_bt = stock if name == 'bluetoothd' else ((work / 'signed-media-inputs/btaudio-stock').read_bytes() if name == 'BTAudioHALPlugin' else None)
        bt[name] = identity(files[path], original_bt, bundle=name == 'BTAudioHALPlugin', retained_special=name == 'BTAudioHALPlugin')
    if bt != r['bluetooth']['identities']:
        raise ValueError('Bluetooth signed identity differs')
    stock_hash = hashlib.sha256(blobs(stock)[0]).digest()[:20]
    metadata = [m[i+20:i+24] for m in modules[:2] for i in range(24, len(m), 24) if m[i:i+20] == stock_hash]
    if metadata != [bytes([2, 0, 1, 0])]:
        raise ValueError('Original Bluetooth trust metadata differs')
    ids = {bytes.fromhex(i['cdhash']): metadata[0] if n == 'bluetoothd' else bytes([2, 0, 0, 0]) for n, i in bt.items()}
    modules.append(trust_module(ids, 'picasso:bluetooth:', ids))
    if media.bundle(modules) != blob:
        raise ValueError('Media trust contains unexpected entries')
    return ui, audio_identity


def report(work):
    verify((work / 'media/media-trust.bundle').read_bytes(), work)
    return json.loads((work / 'media/build-report.json').read_text())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('import', 'check'))
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--run-id')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    if a.mode == 'import':
        if not a.run_id or not re.fullmatch('[1-9][0-9]*', a.run_id):
            raise ValueError('An exact successful Compose run ID is required')
        acquire(a.work.resolve(), a.tree.resolve(), a.run_id)
    else:
        import picasso_audio_payload as audio
        import sys
        audio.check(a.work.resolve(), a.tree.resolve(), a.output.resolve(), profile=sys.modules[__name__])


if __name__ == '__main__':
    main()
