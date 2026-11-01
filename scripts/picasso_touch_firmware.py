"""Pinned stock firmware for the captured g7a_37_02_0a panel (cfg_0)."""
import hashlib
import struct

NAME = 'novatek_nt36672c_g7a_fw01.bin'
URL = 'https://github.com/YinCQL/UniXNU-picasso/releases/download/picasso-touch-firmware-v13.0.5.0/' + NAME
SHA256 = 'f62db7d12c372834e3d0d1ea95665a338a1c64ec955d45e4b91f921ba0690ce2'
SIZE = 131072


def verify(data):
    if len(data) != SIZE or hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Novatek firmware differs from the pinned stock cfg_0 image')
    end = struct.unpack_from('<I', data)[0]
    if not 0x30 <= end <= 0x2000 or (end - 0x30) % 16:
        raise ValueError('Invalid Novatek firmware header')
    if data[-3:] != b'NVT' or data[-4096:-4094] != b'\x14\xeb':
        raise ValueError('Invalid Novatek firmware marker or version')
    # ROM checksums are preserved; the target validates them after SRAM download.
    return dict(name=NAME, bytes=len(data), sha256=SHA256, version=20, panel='g7a_37_02_0a',
                golden_crc=[hex(x) for x in struct.unpack_from('<II', data, 0x18)])
