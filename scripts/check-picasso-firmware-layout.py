#!/usr/bin/env python3
"""Check bundled factory bytes, runtime pins and staging failure boundaries."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import picasso_firmware as firmware

ROOT = firmware.device_root()


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cases = []

    def passed(name):
        cases.append(dict(case=name, result='pass'))

    def rejects(name, operation):
        try:
            operation()
        except (ValueError, FileNotFoundError):
            passed(name)
        else:
            raise AssertionError('Accepted invalid input: ' + name)

    inventory = {}
    for group in firmware.GROUPS:
        pins = firmware.manifest(group)
        if group != 'adsp':
            files = firmware.read_group(group)
            inventory[group] = firmware.stage(out / 'prepared' / group, files, pins)
            passed('factory bytes and staging: ' + group)
    assert sum(row['bytes'] for files in inventory.values() for row in files.values()) == 400944
    passed('twelve bundled files total 400944 bytes')

    board = (ROOT / 'drivers/NativeAdrenoPicasso/Adreno620Board.cpp').read_text()
    for name, expected in firmware.manifest('adreno620')['files'].items():
        match = re.search('"' + re.escape(name) + r'", (\d+)u, 0x([0-9a-f]+)u,\s*\{([^}]+)\}', board)
        assert match and int(match[1]) == expected['bytes'] and int(match[2], 16) == expected['crc32']
        assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', match[3])).hex() == expected['sha256']
    passed('GPU runtime descriptors match all factory pins')
    bt = firmware.manifest('bluetooth')['files']
    legacy = json.loads((ROOT / 'drivers/PicassoBluetooth/firmware.json').read_text())['files']
    runtime = (ROOT / 'drivers/PicassoBluetooth/src/PicassoBtFirmware.c').read_text()
    for name, pin in bt.items():
        assert legacy[name] == dict(size=pin['bytes'], sha256=pin['sha256'])
        match = re.search('"qca/' + re.escape(name) + r'",(\d+),\{([^}]+)\}', runtime)
        assert match and int(match[1]) == pin['bytes']
        assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', match[2])).hex() == pin['sha256']
    passed('Bluetooth legacy and runtime pins match canonical manifest')

    fixture_root = out / 'manifest-fixture'
    target = fixture_root / 'firmware/novatek/manifest.json'
    target.parent.mkdir(parents=True)
    original = firmware.manifest('novatek')
    mutations = [('wrong board', lambda p: p.update(board='sheng')),
                 ('wrong SoC', lambda p: p.update(soc='SM8550')),
                 ('wrong version', lambda p: p.update(stock_version='other')),
                 ('wrong authority', lambda p: p.update(authority='other')),
                 ('path traversal', lambda p: p.update(files={'../escape': next(iter(p['files'].values()))})),
                 ('wrong size type', lambda p: next(iter(p['files'].values())).update(bytes=True)),
                 ('invalid hash', lambda p: next(iter(p['files'].values())).update(sha256='bad')),
                 ('empty inventory', lambda p: p.update(files={})),
                 ('non-object pin', lambda p: p.update(files={'x.bin': 'bad'}))]
    for name, change in mutations:
        pin = copy.deepcopy(original)
        change(pin)
        target.write_text(json.dumps(pin))
        rejects('manifest rejects ' + name, lambda: firmware.manifest('novatek', fixture_root))

    data = {'one.bin': b'first', 'two.bin': b'second'}
    pins = {'files': {name: dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
                     for name, blob in data.items()}}
    invalid = out / 'invalid-stage'
    rejects('bad input publishes no files', lambda: firmware.stage(invalid, dict(data, **{'two.bin': b'bad'}), pins))
    assert not invalid.exists()
    rejects('missing input publishes no files', lambda: firmware.stage(invalid, {'one.bin': data['one.bin']}, pins))
    valid = out / 'valid-stage'
    firmware.stage(valid, data, pins)
    stamps = {p.name: p.stat().st_mtime_ns for p in valid.iterdir()}
    firmware.stage(valid, data, pins)
    assert stamps == {p.name: p.stat().st_mtime_ns for p in valid.iterdir()}
    passed('repeat staging preserves validated output bytes and timestamps')
    broken = out / 'broken-stage'
    broken.mkdir()
    (broken / 'one.bin').write_bytes(b'wrong')
    rejects('bad existing output prevents missing-file publication', lambda: firmware.stage(broken, data, pins))
    assert not (broken / 'two.bin').exists()
    directory = out / 'file-is-directory'
    directory.mkdir()
    rejects('reject directory as firmware file', lambda: firmware.read_file(directory, pins['files']['one.bin']))
    link = out / 'file-link'
    link.symlink_to(valid / 'one.bin')
    rejects('reject symlink as firmware file', lambda: firmware.read_file(link, pins['files']['one.bin']))
    output_link = out / 'directory-link'
    output_link.symlink_to(valid, target_is_directory=True)
    rejects('reject symlink output directory', lambda: firmware.stage(output_link, data, pins))

    # Exercise the actual default CLI with a partial local inventory. It must
    # fail before publishing and must not fetch substitute firmware.
    cli_root = out / 'partial-checkout'
    shutil.copytree(ROOT / 'firmware', cli_root / 'firmware',
                    ignore=lambda path, names: [name for name in names if name != 'manifest.json' and name != 'novatek' and name != 'adreno620' and name != 'bluetooth' and name != 'adsp'])
    gpu_name = 'a650_gmu.bin'
    shutil.copyfile(ROOT / 'firmware/adreno620' / gpu_name, cli_root / 'firmware/adreno620' / gpu_name)
    env = dict(os.environ, UNIXNU_DEVICE=str(cli_root))
    for group, script in [('GPU', 'fetch-picasso-gpu-firmware.py'), ('touch', 'fetch-picasso-touch-firmware.py')]:
        destination = out / ('cli-failed-' + group.lower())
        if group == 'touch':
            destination /= 'firmware.bin'
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts' / script), '--output', str(destination)],
                                env=env, text=True, capture_output=True)
        (out / (group.lower() + '-missing.log')).write_text(result.stdout + result.stderr)
        assert result.returncode != 0 and not destination.exists()
        passed(group + ' CLI rejects missing bundled input before publication')

    # Confirm raw, fill and zero sparse reads across chunk boundaries.
    extractor = module('extract-picasso-firmware')
    sparse_path = out / 'sparse-fixture.img'
    header = struct.pack('<I4H4I', 0xed26ff3a, 1, 0, 28, 12, 4096, 3, 3, 0)
    raw = bytes(range(256)) * 16
    fill = b'abcd' * 1024
    sparse_path.write_bytes(header + struct.pack('<2H2I', 0xcac1, 0, 1, 4108) + raw +
                            struct.pack('<2H2I', 0xcac2, 0, 1, 16) + b'abcd' +
                            struct.pack('<2H2I', 0xcac3, 0, 1, 12))
    sparse = extractor.SparseImage(sparse_path)
    try:
        assert sparse.read(4093, 4103) == (raw + fill + bytes(4096))[4093:8196]
        rejects('sparse out-of-range read', lambda: sparse.read(12288, 1))
    finally:
        sparse.close()
    passed('sparse RAW/FILL/DONTCARE boundary reads')
    sparse_path.write_bytes(sparse_path.read_bytes()[:-1])
    rejects('sparse truncated chunk', lambda: extractor.SparseImage(sparse_path))

    (out / 'results.json').write_text(json.dumps(dict(passed=True, cases=cases, inventory=inventory,
                                                     hardware_tested=False), indent=2) + '\n')
    print('PASS: ' + str(len(cases)) + ' firmware layout checks')


if __name__ == '__main__':
    main()
