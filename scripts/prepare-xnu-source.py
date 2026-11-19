#!/usr/bin/env python3
"""Restore locked public headers, reusing verified blobs across XNU work trees."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUST_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')
DESTINATION = Path('.cache/xnu-work/iokit/DriverKit')
CACHE = Path(f'.cache/sources/xnu-driverkit-{TAG.removeprefix("xnu-")}.json')
URL = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/iokit/DriverKit?ref={TAG}'
BLOB_CACHE = Path('.cache/sources/xnu-header-blobs')


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.' + path.name + '-', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(data)
            stream.flush()
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def open_url(url, timeout=30):
    req = urllib.request.Request(url)
    token = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')
    if token and urllib.parse.urlsplit(url).hostname == 'api.github.com':
        req.add_header('Authorization', f'Bearer {token}')
    req.add_header('User-Agent', 'UniXNU-Source-Prepare')
    return urllib.request.urlopen(req, timeout=timeout)


def download(url):
    for attempt in range(4):
        try:
            with open_url(url, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code not in (408, 429, 500, 502, 503, 504) or attempt == 3:
                raise
            retry = error.headers.get('Retry-After', '') if error.headers else ''
            delay = min(float(retry), 8) if retry.isdigit() else 2 ** attempt
            error.close()
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if attempt == 3:
                raise
            delay = 2 ** attempt
        time.sleep(delay)


def blob_digest(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


def checked_name(entry):
    name = entry.get('relative', entry['name'])
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '\\' in name or path.as_posix() != name:
        raise ValueError('Invalid header cache path')
    if not re.fullmatch('[0-9a-f]{40}', entry['sha']):
        raise ValueError('Invalid locked Git blob')
    return name


def restore(entry, destination=DESTINATION, tag=TAG):
    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
        return None
    name = checked_name(entry)
    target = destination / name
    blob = BLOB_CACHE / entry['sha']
    data = None
    source = 'work-tree'
    if target.is_file():
        data = target.read_bytes()
        if blob_digest(data) != entry['sha']:
            raise ValueError(f'Git blob hash mismatch: {name}')
    elif target.exists():
        raise ValueError(f'Header target is not a file: {name}')
    if data is None and blob.is_file():
        cached = blob.read_bytes()
        if blob_digest(cached) == entry['sha']:
            data, source = cached, 'blob-cache'
    if data is None:
        # Work source is replaced for every overlay fingerprint; retain exact public
        # header bytes without fetching them again from the download service.
        work = Path('.cache/xnu-work')
        try:
            relative = target.relative_to(work)
        except ValueError:
            relative = None
        if relative is not None:
            for prior in sorted(work.parent.glob('xnu-work-before-*'), key=lambda p: p.stat().st_mtime, reverse=True):
                candidate = prior / relative
                if candidate.is_file():
                    cached = candidate.read_bytes()
                    if blob_digest(cached) == entry['sha']:
                        data, source = cached, 'previous-work-tree'
                        break
    if data is None:
        data, source = download(entry['download_url']), 'download'
        if blob_digest(data) != entry['sha']:
            raise ValueError(f'Git blob hash mismatch: {name}')
    if not blob.is_file() or blob.read_bytes() != data:
        atomic_write(blob, data)
    restored = not target.is_file()
    if restored:
        atomic_write(target, data)
    return {'name': name, 'destination': str(destination), 'source_tag': tag,
            'git_blob': entry['sha'], 'sha256': hashlib.sha256(data).hexdigest(),
            'source': source, 'restored': restored}


def metadata(path, url, bundled):
    if path.exists():
        return json.loads(path.read_text())
    data = bundled.read_bytes() if bundled.is_file() else download(url)
    value = json.loads(data)
    if not isinstance(value, list):
        raise ValueError('Header manifest is not a list')
    atomic_write(path, data)
    return value


def main():
    if not DESTINATION.is_dir():
        raise SystemExit('Create the local XNU work source before preparing headers')
    bundled = Path(__file__).resolve().parent / 'headers-cache'
    crypto_cache = CACHE.with_name(f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json')
    trust_cache = CACHE.with_name(f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json')
    entries = metadata(CACHE, URL, bundled / CACHE.name)
    for item in metadata(crypto_cache, URL.replace('/DriverKit?', '/DriverKit/crypto?'), bundled / crypto_cache.name):
        entry = dict(item)
        entry['relative'] = 'crypto/' + entry['name']
        entries.append(entry)
    trust_url = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/EXTERNAL_HEADERS/TrustCache?ref={TRUST_TAG}'
    trust = metadata(trust_cache, trust_url, bundled / trust_cache.name)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        restored = [entry for entry in executor.map(restore, entries) if entry]
        def restore_trust(entry):
            return restore(entry, Path('.cache/xnu-work/EXTERNAL_HEADERS/TrustCache'), TRUST_TAG)
        restored.extend(entry for entry in executor.map(restore_trust, trust) if entry)
    previous = Path('.cache/sources/xnu-restored-headers.json')
    atomic_write(previous, (json.dumps(restored, indent=2) + '\n').encode())
    counts = {source: sum(entry['source'] == source for entry in restored)
              for source in ('work-tree', 'blob-cache', 'previous-work-tree', 'download')}
    print(f'Verified {len(restored)} locked public headers; restored {sum(entry["restored"] for entry in restored)}; sources {counts}')


if __name__ == '__main__':
    main()
