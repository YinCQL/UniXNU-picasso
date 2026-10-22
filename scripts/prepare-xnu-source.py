#!/usr/bin/env python3
"""Restore omitted DriverKit headers from Apple's earlier Darwin 23 source."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import urllib.request

# Tags of the Apple sources that still carry the headers newer xnu omits; feeds.toml sets them for the unixnu driver.
TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUST_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')
DESTINATION = Path('.cache/xnu-work/iokit/DriverKit')
CACHE = Path(f'.cache/sources/xnu-driverkit-{TAG.removeprefix("xnu-")}.json')
URL = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/iokit/DriverKit?ref={TAG}'


def open_url(url, timeout=30):
    req = urllib.request.Request(url)
    token = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')
    if token and 'api.github.com' in url:
        req.add_header('Authorization', f'Bearer {token}')
    req.add_header('User-Agent', 'UniXNU-Source-Prepare')
    return urllib.request.urlopen(req, timeout=timeout)


def restore(entry, destination=DESTINATION, tag=TAG):
    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
        return None
    name = entry.get('relative', entry['name'])
    target = destination / name
    if target.exists():
        return None
    data = open_url(entry['download_url'], timeout=30).read()
    digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    if digest != entry['sha']:
        raise ValueError(f'Git blob hash mismatch: {entry["name"]}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {'name': name, 'source_tag': tag, 'git_blob': digest,
            'sha256': hashlib.sha256(data).hexdigest()}


if __name__ == '__main__':
    if not DESTINATION.is_dir():
        raise SystemExit('Create the local XNU work source before preparing headers')

    bundled_dir = Path(__file__).resolve().parent / 'headers-cache'
    crypto_cache = CACHE.with_name(f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json')
    trust_cache = CACHE.with_name(f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json')

    # Seed from bundled cache if available to avoid API rate limits
    for target_cache, bundled_name in [
        (CACHE, f'xnu-driverkit-{TAG.removeprefix("xnu-")}.json'),
        (crypto_cache, f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json'),
        (trust_cache, f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json'),
    ]:
        if not target_cache.exists() and (bundled_dir / bundled_name).is_file():
            target_cache.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(bundled_dir / bundled_name, target_cache)

    if not CACHE.exists():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        CACHE.write_bytes(open_url(URL, timeout=30).read())
    entries = json.loads(CACHE.read_text())

    if not crypto_cache.exists():
        crypto_url = URL.replace('/DriverKit?', '/DriverKit/crypto?')
        crypto_cache.write_bytes(open_url(crypto_url, timeout=30).read())
    for entry in json.loads(crypto_cache.read_text()):
        entry['relative'] = 'crypto/' + entry['name']
        entries.append(entry)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        restored = [entry for entry in executor.map(restore, entries) if entry]
        trust_tag = TRUST_TAG
        if not trust_cache.exists():
            url = ('https://api.github.com/repos/apple-oss-distributions/xnu/'
                   f'contents/EXTERNAL_HEADERS/TrustCache?ref={trust_tag}')
            trust_cache.write_bytes(open_url(url, timeout=30).read())
        trust_destination = Path('.cache/xnu-work/EXTERNAL_HEADERS/TrustCache')

        def restore_trust(entry):
            return restore(entry, trust_destination, trust_tag)

        restored.extend(entry for entry in executor.map(
            restore_trust, json.loads(trust_cache.read_text())) if entry)

    previous = Path('.cache/sources/xnu-restored-headers.json')
    existing = json.loads(previous.read_text()) if previous.exists() else []
    by_name = {entry['name']: entry for entry in existing + restored}
    previous.write_text(json.dumps(list(by_name.values()), indent=2) + '\n')
    print(f'Restored {len(restored)} omitted public headers from Apple; blob hashes verified')
