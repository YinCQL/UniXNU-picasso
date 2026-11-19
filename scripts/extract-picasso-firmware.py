#!/usr/bin/env python3
"""Extract pinned firmware from the Picasso V13.0.5.0.SGICNXM factory images."""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import picasso_firmware as firmware


def checked_container(path, pin):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != pin['bytes']:
        raise ValueError('Invalid stock container: ' + str(path))
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while data := source.read(4 * 1024 * 1024):
            digest.update(data)
    if digest.hexdigest() != pin['sha256']:
        raise ValueError('Stock container hash mismatch: ' + path.name)


class SparseImage:
    def __init__(self, path):
        self.stream = path.open('rb')
        try:
            self.parse(path)
        except Exception:
            self.close()
            raise

    def parse(self, path):
        data = self.stream.read(28)
        if len(data) != 28:
            raise ValueError('Truncated stock sparse header')
        magic, major, minor, header, chunk_header, block, blocks, count, checksum = struct.unpack('<I4H4I', data)
        if magic != 0xed26ff3a or major != 1 or header != 28 or chunk_header != 12 or block != 4096 or count > 65536:
            raise ValueError('Unsupported stock sparse image')
        self.chunks = []
        position = header
        logical = 0
        for _ in range(count):
            self.stream.seek(position)
            data = self.stream.read(12)
            if len(data) != 12:
                raise ValueError('Truncated stock sparse chunk header')
            kind, reserved, chunk_blocks, total = struct.unpack('<2H2I', data)
            size = chunk_blocks * block
            payload = size if kind == 0xcac1 else 4 if kind in (0xcac2, 0xcac4) else 0
            if kind not in (0xcac1, 0xcac2, 0xcac3, 0xcac4) or total != 12 + payload or position + total > path.stat().st_size:
                raise ValueError('Invalid stock sparse chunk')
            if kind == 0xcac4:
                if size:
                    raise ValueError('Invalid sparse CRC chunk')
            elif size:
                self.chunks.append((logical, logical + size, kind, position + 12))
                logical += size
            position += total
        if logical != blocks * block or position != path.stat().st_size:
            raise ValueError('Sparse extent or trailing data mismatch')
        self.starts = [chunk[0] for chunk in self.chunks]
        self.size = logical

    def close(self):
        self.stream.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError('Sparse read outside image')
        output = bytearray()
        while size:
            lo, hi, kind, source = self.chunks[bisect.bisect_right(self.starts, offset) - 1]
            take = min(size, hi - offset)
            if kind == 0xcac1:
                self.stream.seek(source + offset - lo)
                data = self.stream.read(take)
            elif kind == 0xcac2:
                self.stream.seek(source)
                value = self.stream.read(4)
                start = (offset - lo) % 4
                data = (value * ((take + start + 3) // 4))[start:start + take]
            else:
                data = bytes(take)
            if len(data) != take:
                raise ValueError('Truncated sparse data')
            output.extend(data)
            offset += take
            size -= take
        return bytes(output)


def gpu_files(images, pins, scratch):
    pin = pins['container']
    source = images / pin['name']
    checked_container(source, pin)
    sparse = SparseImage(source)
    vendor = scratch / 'vendor.img'
    digest = hashlib.sha256()
    # Extent is pinned to the verified stock LP metadata; arbitrary partitions
    # and other device layouts are not accepted by this extractor.
    offset, remaining = pin['first_sector'] * 512, pin['sectors'] * 512
    try:
        with vendor.open('xb') as output:
            while remaining:
                take = min(4 * 1024 * 1024, remaining)
                data = sparse.read(offset, take)
                output.write(data)
                digest.update(data)
                offset += take
                remaining -= take
    finally:
        sparse.close()
    if digest.hexdigest() != pin['partition_sha256']:
        raise ValueError('Extracted stock vendor partition differs')
    files = {}
    for name, expected in pins['files'].items():
        output = scratch / name
        subprocess.run(['debugfs', '-R', 'dump /firmware/' + name + ' ' + str(output), str(vendor)],
                       check=True, capture_output=True)
        files[name] = firmware.read_file(output, expected)
    return files


def extract(stock_dir, output, groups):
    stock_dir, output = Path(stock_dir), Path(output)
    images = stock_dir / 'images'
    if not images.is_dir():
        raise ValueError('Expected the factory directory containing images/')
    prepared = {}
    # Gather and verify every requested group before publishing outputs.
    with tempfile.TemporaryDirectory(prefix='picasso-stock-') as temporary:
        scratch = Path(temporary)
        for group in dict.fromkeys(groups):
            pins = firmware.manifest(group)
            if group == 'novatek':
                pin = pins['container']
                path = images / pin['name']
                checked_container(path, pin)
                data = path.read_bytes()
                if data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 36)[0] != pin['kernel_offset']:
                    raise ValueError('Stock boot header differs')
                offset = pin['kernel_offset'] + pin['firmware_kernel_offset']
                name, = pins['files']
                files = {name: firmware.verify(data[offset:offset + pins['files'][name]['bytes']], pins['files'][name], name)}
            elif group == 'adreno620':
                files = gpu_files(images, pins, scratch)
            elif group == 'bluetooth':
                files = firmware.bluetooth_files(btfm=images / pins['container']['name'])
            else:
                files = {name: firmware.read_file(images / name, pin) for name, pin in pins['files'].items()}
            prepared[group] = (files, pins)
    result = {}
    for group, (files, pins) in prepared.items():
        result[group] = firmware.stage(output / group, files, pins)
    return dict(schema='picasso-stock-extracted-v1', stock_version='V13.0.5.0.SGICNXM', files=result, hardware_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=firmware.device_root() / 'firmware')
    parser.add_argument('--group', action='append', choices=firmware.GROUPS)
    args = parser.parse_args()
    report = extract(args.stock_dir, args.output, args.group or firmware.GROUPS)
    target = firmware.device_root() / 'out/picasso/stock-firmware.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: extracted exact V13.0.5.0.SGICNXM firmware: ' + ', '.join(report['files']))


if __name__ == '__main__':
    main()
