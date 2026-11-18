"""Exact factory V13.0.5.0 Adreno 620 inputs, bundled in firmware/adreno620."""
import hashlib
import io
import json
from pathlib import Path
import stat
import zipfile
import zlib

from picasso_firmware import manifest

PINS = manifest('adreno620')
VERSION = PINS['stock_version']
ARCHIVE_BYTES = PINS['archive']['bytes']
ARCHIVE_SHA256 = PINS['archive']['sha256']
FILES = {name: (row['bytes'], row['sha256'], row['crc32'])
         for name, row in PINS['files'].items()}


def verify(name, data):
    size, digest, crc = FILES[name]
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest or zlib.crc32(data) != crc:
        raise ValueError(f'GPU firmware differs from pinned {VERSION}: {name}')
    return dict(bytes=size, sha256=digest, crc32=crc)


def verify_directory(directory):
    root = Path(directory)
    if root.is_symlink() or not root.is_dir():
        raise ValueError('GPU firmware directory missing; run fetch-picasso-gpu-firmware.py')
    result = {}
    for name in FILES:
        path = root / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != FILES[name][0]:
            raise ValueError(f'Missing or invalid GPU firmware: {name}')
        result[name] = verify(name, path.read_bytes())
    return dict(schema='picasso-adreno620-firmware-v1', stock_version=VERSION,
                files=result, hardware_validated=False)


def unpack(data):
    if len(data) != ARCHIVE_BYTES or hashlib.sha256(data).hexdigest() != ARCHIVE_SHA256:
        raise ValueError('GPU firmware archive differs from pinned local archive')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if len(entries) != len(FILES) + 1 or {e.filename for e in entries} != set(FILES) | {'manifest.json'}:
            raise ValueError('Unexpected GPU firmware archive inventory')
        for entry in entries:
            limit = FILES[entry.filename][0] if entry.filename in FILES else 16 * 1024
            mode = entry.external_attr >> 16
            if entry.flag_bits & 1 or not stat.S_ISREG(mode) or entry.file_size > limit:
                raise ValueError('Invalid GPU firmware archive member')
        contents = {name: archive.read(name) for name in FILES}
        expected = {name: verify(name, blob) for name, blob in contents.items()}
        manifest = json.loads(archive.read('manifest.json'))
        if (manifest['schema'] != 'picasso-adreno620-firmware-v1' or
                manifest['stock_version'] != VERSION or manifest['files'] != expected):
            raise ValueError('GPU firmware archive manifest mismatch')
    return contents
