#!/usr/bin/env python3
"""Add or replace one file in an existing NGPUADD2 GPU addon.

  add-gpu-addon-file.py INPUT ADDONPATH MODE SOURCE OUTPUT

The kernel's addon installer writes every entry below /private/var/ngpu, so a
path like libpicasso-sb.dylib is the runtime /private/var/ngpu/libpicasso-sb.dylib.
This exists to carry the SpringBoard inject library without changing the Data
seed (whose identity keys the persistent store).
"""
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b'NGPUADD2'


def parse(blob):
    if blob[:8] != MAGIC or len(blob) < 24:
        raise ValueError('not an NGPUADD2 addon')
    count, reserved, total = struct.unpack_from('<IIQ', blob, 8)
    if reserved or total != len(blob) or count > 4096:
        raise ValueError('invalid addon header')
    at = 24
    files = []
    for _ in range(count):
        if len(blob) - at < 24:
            raise ValueError('truncated addon entry')
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name_pad = (name_bytes + 7) & ~7
        data_pad = (packed + 7) & ~7
        if name_bytes < 2 or name_bytes > 160 or len(blob) - at < name_pad + data_pad:
            raise ValueError('invalid addon entry')
        name = blob[at:at + name_bytes]
        if name[-1] != 0 or b'\0' in name[:-1]:
            raise ValueError('invalid addon path')
        at += name_pad
        data = zlib.decompress(blob[at:at + packed])
        if len(data) != size:
            raise ValueError('addon entry size mismatch')
        at += data_pad
        files.append((name[:-1].decode(), mode, data))
    if at != len(blob):
        raise ValueError('trailing addon bytes')
    return files


def build(files):
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or name.startswith('/') or '..' in name.split('/'):
            raise ValueError('bad addon path ' + name)
        packed = zlib.compress(data, 9)
        name_pad = encoded + bytes(-len(encoded) % 8)
        data_pad = packed + bytes(-len(packed) % 8)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + name_pad + data_pad
    return MAGIC + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def add(input_path, output_path, name, mode, source_path):
    files = parse(Path(input_path).read_bytes())
    data = Path(source_path).read_bytes()
    files = [(n, m, d) for (n, m, d) in files if n != name]
    files.append((name, mode, data))
    Path(output_path).write_bytes(build(files))
    print(f'{output_path}: {len(files)} files, replaced {name} ({len(data)} bytes)')


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    add(sys.argv[1], sys.argv[5], sys.argv[2], int(sys.argv[3], 8), sys.argv[4])


if __name__ == '__main__':
    main()
