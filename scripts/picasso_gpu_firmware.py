"""Exact stock V13.0.5.0 Adreno 620 inputs; no firmware bytes live in Git."""
import hashlib
import io
import json
from pathlib import Path
import stat
import zipfile
import zlib

VERSION = 'V13.0.5.0.SGICNXM'
ARCHIVE_BYTES = 72163
ARCHIVE_SHA256 = 'bc8b240a1d1220371ded8f3d103f75682d68f97f2506f3d731b242bb853665e9'
URL = 'https://github.com/YinCQL/UniXNU-picasso/releases/download/picasso-gpu-firmware-v13.0.5.0/picasso-adreno620-v13.0.5.0.zip'
FILES = {
    'a650_gmu.bin': (41548, 'df79fa8482fcc7587bb239b9c9e2b24bce4729893b990fe3f82b30da8175b01c', 0x5c66275c),
    'a650_sqe.fw': (31632, 'b4c27420fd6a5c6b511b254cc0fad8cdd40eea38ceb3418f895a1084f4460487', 0x1f3285db),
    'a630_sqe.fw': (32264, '57eece0d8032b0ab2467d856c304ce22f502ae8d4b2f8169b9d1ca890ceb463c', 0xf9a2972e),
    'a620_zap.mdt': (6860, '5f9b1c2804a4c680d03ebc7f1e1d87995d2f1c96fa1acfe87aaba1003711eba2', 0x7de1dd05),
    'a620_zap.b00': (148, 'ec9b9d5a67456384809624b14a00d15d36297e5f2e75728504cd872bc95e0947', 0x4629d3f3),
    'a620_zap.b01': (6712, '580bf91b7e582b3c513ee09292da74d1d93d5513daa8dc28bea7f092dd43670a', 0x41ba9c58),
    'a620_zap.b02': (1676, '5a4de8ad881b57d64c3697b1078ce012fb47300d914a32c50522da762e2f2bd8', 0xb439bc29),
    'a620_zap.elf': (13964, 'd7d7c4a019945850e37ed4370c6f6fc5b763b3933179379a366090c87f5e39b6', 0xc2f2ba0c),
}


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
        raise ValueError('GPU firmware archive differs from fixed Release')
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
