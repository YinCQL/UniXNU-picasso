#!/usr/bin/env python3
"""Read macOS-attached original root disks and verify their signed record tables.

Attaches only the pinned input images, read-only and without mounting a volume.
No root image is generated, no private key is used and no partition is written.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import plistlib
import re
import runpy
import subprocess
import sys

import picasso_target as target

ROOT = Path(__file__).resolve().parents[1]
RECORD_BYTES = 131072


@contextmanager
def attached_disk(image):
    attached = plistlib.loads(subprocess.check_output([
        'hdiutil', 'attach', '-nomount', '-readonly', '-noautofsck', '-plist', str(image)]))
    entities = attached['system-entities']
    whole = [entry['dev-entry'] for entry in entities
             if re.fullmatch(r'/dev/disk[0-9]+', entry.get('dev-entry', ''))]
    if not whole:
        raise ValueError('Missing whole disk in the read-only image attachment')
    # Match the upstream userdata builder: the first whole disk is the attached
    # image. APFS may also list a synthesized container later in these entities.
    device = whole[0]
    try:
        if any(entry.get('mount-point') for entry in entities):
            raise ValueError('Root input was unexpectedly mounted')
        yield '/dev/r' + device.removeprefix('/dev/')
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)


def verify_stream(stream, expected, manifest):
    count = (expected['size'] + RECORD_BYTES - 1) // RECORD_BYTES
    if len(manifest) != 544 + count * 32:
        raise ValueError('Signed root manifest length mismatch')
    remaining, image_hash = expected['size'], hashlib.sha256()
    for index in range(count):
        want = min(RECORD_BYTES, remaining)
        data = bytearray()
        while len(data) < want:
            part = stream.read(want - len(data))
            if not part:
                raise ValueError(f'{expected["name"]}: short read in record {index}')
            data.extend(part)
        if hashlib.sha256(data).digest() != manifest[544 + index * 32:544 + (index + 1) * 32]:
            raise ValueError(f'{expected["name"]}: native disk differs from signed record {index}')
        image_hash.update(data)
        remaining -= want
    if remaining or image_hash.hexdigest() != expected['sha256']:
        raise ValueError(f'{expected["name"]}: native whole-disk SHA256 mismatch')
    return dict(name=expected['name'], bytes=expected['size'], sha256=image_hash.hexdigest(),
                signed_records_checked=count, matches_signed_policy=True)


def check(inputs, output):
    if sys.platform != 'darwin':
        raise ValueError('Native root input comparison requires macOS hdiutil')
    if output.exists():
        raise ValueError('Use a new root input report path')
    policy = ROOT / 'policy/verified-root-picasso'
    runpy.run_path(str(ROOT / 'scripts/check-picasso-target.py'))['check_policy'](policy)
    report = dict(passed=False, native_hdiutil=False, windows=[],
                  verification_method='macOS hdiutil read-only whole disks',
                  input_images_readonly=True, partition_write_operations=False, runtime_qualified=False)
    try:
        for expected in target.ROOT_WINDOWS:
            component = 'OS' if expected['name'] == 'OS' else 'Cryptex1,SystemOS'
            member, pin = target.COMPONENTS[component]
            source = inputs / Path(member).name
            with source.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != pin:
                    raise ValueError(f'{component}: original image SHA256 mismatch')
            info = plistlib.loads(subprocess.check_output(['hdiutil', 'imageinfo', '-plist', str(source)]))
            if info['Size Information']['Total Bytes'] != expected['size']:
                raise ValueError(f'{component}: native whole-disk size differs from the signed policy')
            with attached_disk(source) as raw:
                with open(raw, 'rb', buffering=0) as stream:
                    result = verify_stream(stream, expected, (policy / expected['directory'] / 'manifest.bin').read_bytes())
            report['windows'].append(result)
        report['passed'] = report['native_hdiutil'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.inputs.resolve(), args.output.resolve()), indent=2))


if __name__ == '__main__':
    main()
