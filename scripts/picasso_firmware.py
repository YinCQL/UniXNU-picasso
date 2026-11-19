"""Load board-owned firmware pins and stage verified local inputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import zlib

SCHEMA = 'picasso-firmware-input-v1'
GROUPS = ('novatek', 'adreno620', 'bluetooth', 'adsp')


def device_root():
    selected = os.environ.get('UNIXNU_DEVICE')
    if selected:
        root = Path(selected)
        if not (root / 'firmware/novatek/manifest.json').is_file():
            raise ValueError('UNIXNU_DEVICE has no Picasso firmware manifests')
        return root
    root = Path(__file__).resolve().parents[1]
    for candidate in (root, *root.parents):
        if (candidate / 'firmware/novatek/manifest.json').is_file():
            return candidate
    raise ValueError('Picasso firmware manifests are missing')


def manifest(group, root=None):
    if group not in GROUPS:
        raise ValueError('Unknown Picasso firmware group: ' + group)
    data = json.loads(((root or device_root()) / 'firmware' / group / 'manifest.json').read_text())
    if (data.get('schema') != SCHEMA or data.get('board') != 'picasso' or
            data.get('soc') != 'SM7250' or data.get('stock_version') != 'V13.0.5.0.SGICNXM' or
            data.get('authority') != 'picasso_images_V13.0.5.0.SGICNXM' or
            not isinstance(data.get('files'), dict) or not data['files']):
        raise ValueError('Invalid Picasso firmware manifest: ' + group)
    for name, expected in data['files'].items():
        if (not isinstance(expected, dict) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', name) or
                type(expected.get('bytes')) is not int or not 0 < expected['bytes'] <= 256 * 1024 * 1024 or
                not re.fullmatch(r'[0-9a-f]{64}', expected.get('sha256', ''))):
            raise ValueError('Invalid firmware file pin: ' + name)
        if 'crc32' in expected and (type(expected['crc32']) is not int or not 0 <= expected['crc32'] <= 0xffffffff):
            raise ValueError('Invalid firmware CRC pin: ' + name)
    return data


def verify(data, expected, label):
    if (len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256'] or
            ('crc32' in expected and zlib.crc32(data) != expected['crc32'])):
        raise ValueError('Firmware identity mismatch: ' + label)
    return data


def read_file(path, expected):
    path = Path(path)
    if not stat.S_ISREG(path.lstat().st_mode) or path.stat().st_size != expected['bytes']:
        raise ValueError('Missing or invalid firmware file: ' + str(path))
    return verify(path.read_bytes(), expected, path.name)


def read_group(group, directory=None):
    pins = manifest(group)
    directory = Path(directory) if directory is not None else device_root() / 'firmware' / group
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('Missing or invalid firmware directory: ' + str(directory))
    return {name: read_file(directory / name, expected) for name, expected in pins['files'].items()}


def stage(directory, files, pins):
    """Validate every input and existing output before publishing missing files."""
    directory = Path(directory)
    if set(files) != set(pins['files']):
        raise ValueError('Firmware inventory differs from manifest')
    for name, data in files.items():
        verify(data, pins['files'][name], name)
    if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
        raise ValueError('Invalid firmware output directory')
    for name, data in files.items():
        target = directory / name
        if os.path.lexists(target):
            if read_file(target, pins['files'][name]) != data:
                raise ValueError('Existing firmware output differs: ' + name)
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        target = directory / name
        if os.path.lexists(target):
            read_file(target, pins['files'][name])
            continue
        with tempfile.NamedTemporaryFile(dir=directory, prefix='.' + name + '-', delete=False) as stream:
            temporary = Path(stream.name)
            if hasattr(os, 'fchmod'):
                os.fchmod(stream.fileno(), 0o644)
            else:
                os.chmod(temporary, 0o644)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            # Publish without replacing an input created by a concurrent process.
            try:
                os.link(temporary, target)
            except FileExistsError:
                read_file(target, pins['files'][name])
        finally:
            temporary.unlink(missing_ok=True)
    return {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            for name, data in files.items()}


def bluetooth_files(source=None, btfm=None):
    pins = manifest('bluetooth')
    if source is not None and btfm is not None:
        raise ValueError('Choose a Bluetooth directory or BTFM image')
    if btfm is None:
        return read_group('bluetooth', source)
    from picasso_adsp_firmware import Fat16
    volume = Fat16(read_file(btfm, pins['container']))
    entries = volume.entries(volume.root)
    if 'image' not in entries or not entries['image'][2]:
        raise ValueError('BTFM image directory is missing')
    entries = volume.entries(volume.chain(entries['image'][0]))
    files = {}
    for name, expected in pins['files'].items():
        if name not in entries or entries[name][2] or entries[name][1] != expected['bytes']:
            raise ValueError('BTFM firmware entry differs: ' + name)
        files[name] = verify(volume.chain(entries[name][0], entries[name][1]), expected, name)
    return files
