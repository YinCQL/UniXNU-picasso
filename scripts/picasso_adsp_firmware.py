"""Picasso ADSP input preparation, following power.md and QcomMdt.c's split layout.

The stock NON-HLOS is a tail-trimmed FAT16 image. Only unallocated trailing
space may be missing: each directory, FAT and file byte must exist in the input.
No vendor binaries are stored in Git. This prepares PAS input, not a PAS result.
"""
import hashlib
from pathlib import Path
import struct

SOURCE_BYTES = 178958336
SOURCE_SHA256 = 'a77ac6adc2f89300ce953d2b5e0e52d6f9263e41bb23c424aaaf9a26bf20a8be'
BASE, SIZE = 0x89200000, 0x02800000
MAX_FILE = SIZE


def require(test, why):
    if not test:
        raise ValueError(why)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Fat16:
    def __init__(self, data):
        self.data = data
        b = self.read(0, 512)
        self.bps, self.spc, reserved, fats, root, total = struct.unpack_from('<HBHBHH', b, 11)
        fat_sectors = struct.unpack_from('<H', b, 22)[0]
        if not total:
            total = struct.unpack_from('<I', b, 32)[0]
        require(b[510:512] == b'\x55\xaa' and self.bps in (512, 1024, 2048, 4096) and
                0 < self.spc <= 128 and not self.spc & (self.spc - 1) and
                reserved and fats == 2 and root and fat_sectors, 'FAT16 geometry')
        root_sectors = (root * 32 + self.bps - 1) // self.bps
        data_sector = reserved + fats * fat_sectors + root_sectors
        self.clusters = (total - data_sector) // self.spc
        require(4085 <= self.clusters < 65525 and
                fat_sectors * self.bps >= (self.clusters + 2) * 2, 'FAT16 cluster table')
        self.cluster_bytes = self.bps * self.spc
        self.data_offset = data_sector * self.bps
        self.fat = self.read(reserved * self.bps, fat_sectors * self.bps)
        require(self.fat == self.read((reserved + fat_sectors) * self.bps, len(self.fat)), 'FAT mirrors differ')
        self.root = self.read((reserved + fats * fat_sectors) * self.bps, root * 32)
        self.logical_bytes = total * self.bps
        require(len(data) <= self.logical_bytes, 'Data beyond declared FAT volume')

    def read(self, offset, size):
        require(0 <= offset <= len(self.data) and 0 <= size <= len(self.data) - offset, 'Truncated allocated FAT data')
        return self.data[offset:offset + size]

    def chain(self, first, size=None):
        require(size is None or 0 < size <= MAX_FILE, 'File size bound')
        seen, chunks, copied = set(), [], 0
        cluster = first
        limit = 128 if size is None else (size + self.cluster_bytes - 1) // self.cluster_bytes
        while True:
            require(2 <= cluster < self.clusters + 2 and cluster not in seen and len(seen) < limit,
                    'Invalid, cyclic or overlong FAT chain')
            seen.add(cluster)
            take = self.cluster_bytes if size is None else min(self.cluster_bytes, size - copied)
            chunks.append(self.read(self.data_offset + (cluster - 2) * self.cluster_bytes, take))
            copied += take
            cluster = struct.unpack_from('<H', self.fat, cluster * 2)[0]
            if cluster >= 0xfff8:
                break
        result = b''.join(chunks)
        require(size is None or len(result) == size, 'Short FAT chain')
        return result

    @staticmethod
    def entries(data):
        result, long = {}, []
        for at in range(0, len(data), 32):
            d = data[at:at + 32]
            require(len(d) == 32, 'Short directory entry')
            if d[0] == 0:
                break
            if d[0] == 0xe5:
                long = []
                continue
            if d[11] == 15:
                if d[0] & 0x40:
                    long = []
                long.append(d)
                require(len(long) <= 20, 'Long name bound')
                continue
            if d[11] & 8:
                long = []
                continue
            name = d[:8].decode('ascii').rstrip()
            ext = d[8:11].decode('ascii').rstrip()
            if ext:
                name += '.' + ext
            if long:
                checksum = 0
                for c in d[:11]:
                    checksum = (((checksum & 1) << 7) + (checksum >> 1) + c) & 255
                units = []
                for i, part in enumerate(reversed(long), 1):
                    require(part[0] == (i | (0x40 if i == len(long) else 0)) and
                            part[12] == 0 and part[13] == checksum and part[26:28] == b'\0\0', 'Invalid FAT long name')
                    units += [struct.unpack_from('<H', part, p)[0] for p in (1,3,5,7,9,14,16,18,20,22,24,28,30)]
                end = units.index(0) if 0 in units else len(units)
                require(all(n == 0xffff for n in units[end + 1:]) and all(0 < n < 128 for n in units[:end]), 'Non-ASCII firmware path')
                name = ''.join(map(chr, units[:end]))
            long = []
            if name in ('.', '..'):
                continue
            require(name and '/' not in name and '\\' not in name and name.lower() not in result, 'Duplicate/unsafe firmware path')
            require(d[20:22] == b'\0\0', 'FAT16 high cluster')
            result[name.lower()] = (struct.unpack_from('<H', d, 26)[0], struct.unpack_from('<I', d, 28)[0], bool(d[11] & 16))
        return result

    def firmware(self):
        root = self.entries(self.root)
        require('image' in root and root['image'][2], 'No firmware image directory')
        entries = self.entries(self.chain(root['image'][0]))
        files = {}
        for name, (first, size, directory) in entries.items():
            if name == 'adsp.mdt' or (name.startswith('adsp.b') and len(name) == 8 and name[-2:].isdigit()):
                require(not directory, 'Firmware is a directory')
                files[name] = self.chain(first, size)
        return files


def join(files):
    require('adsp.mdt' in files and 'adsp.b00' in files and 'adsp.b01' in files, 'Missing ADSP headers')
    mdt = files['adsp.mdt']
    require(len(mdt) >= 52 and mdt[:7] == b'\x7fELF\x01\x01\x01' and
            struct.unpack_from('<H', mdt, 18)[0] == 164 and struct.unpack_from('<I', mdt, 20)[0] == 1,
            'Expected little-endian ELF32 Hexagon')
    phoff = struct.unpack_from('<I', mdt, 28)[0]
    ehsize, entsize, count = struct.unpack_from('<HHH', mdt, 40)
    require(ehsize == 52 and phoff == 52 and entsize == 32 and 3 <= count <= 64 and
            phoff + count * entsize <= len(mdt), 'ADSP program headers')
    segments, extent, low, high, hashes, expected = [], 0, BASE + SIZE, BASE, [], {'adsp.mdt'}
    for i in range(count):
        typ, off, va, pa, size, mem, flags, align = struct.unpack_from('<8I', mdt, phoff + i * 32)
        require(off <= MAX_FILE and size <= MAX_FILE - off, 'ADSP file extent')
        is_hash = flags & 0x07000000 == 0x02000000
        load = typ == 1 and not is_hash
        if is_hash:
            hashes.append(i)
        if load:
            require(0 < mem and size <= mem and BASE <= pa < BASE + SIZE and mem <= BASE + SIZE - pa and
                    flags & 0x08000000 and (align in (0, 1) or not align & (align - 1)), 'ADSP memory/relocation range')
            for previous in segments:
                if previous['load']:
                    require(pa + mem <= previous['address'] or previous['address'] + previous['memory_bytes'] <= pa,
                            'Overlapping ADSP memory segments')
            low, high = min(low, pa), max(high, pa + mem)
        if size:
            name = f'adsp.b{i:02d}'
            expected.add(name)
            require(name in files and len(files[name]) == size, f'Missing/short ADSP piece {i}')
            for previous in segments:
                if previous['file_bytes']:
                    require(off + size <= previous['offset'] or previous['offset'] + previous['file_bytes'] <= off,
                            'Overlapping ADSP file segments')
            extent = max(extent, off + size)
        segments.append(dict(index=i, offset=off, address=pa, file_bytes=size, memory_bytes=mem, flags=flags, load=bool(load)))
    require(hashes == [1] and segments[0]['offset'] == 0 and not segments[0]['load'] and
            segments[0]['file_bytes'] >= phoff + count * 32, 'ADSP metadata layout')
    require(set(files) == expected and mdt == files['adsp.b00'] + files['adsp.b01'], 'Split metadata/pieces differ')
    require(low == BASE and high == BASE + SIZE, 'Unexpected Picasso ADSP carveout')
    image = bytearray(extent)
    for s in segments:
        if s['file_bytes']:
            image[s['offset']:s['offset'] + s['file_bytes']] = files[f'adsp.b{s["index"]:02d}']
    return bytes(image), dict(pas_id=1, machine='Hexagon', reserved_base=BASE, reserved_bytes=SIZE,
        segments=segments, metadata_bytes=len(mdt), metadata_sha256=digest(mdt),
        image_bytes=len(image), image_sha256=digest(image), authenticated=False,
        protocol='APR', channel='apr_audio_svc', files={n: dict(bytes=len(b), sha256=digest(b)) for n,b in sorted(files.items())})


def prepare(source):
    source = Path(source)
    require(not source.is_symlink() and source.is_file() and source.stat().st_size == SOURCE_BYTES, 'Expected stock NON-HLOS.bin size')
    data = source.read_bytes()
    require(digest(data) == SOURCE_SHA256, 'NON-HLOS differs from supplied Picasso stock firmware')
    volume = Fat16(data)
    files = volume.firmware()
    image, report = join(files)
    report.update(source_sha256=SOURCE_SHA256, source_bytes=len(data), fat_logical_bytes=volume.logical_bytes,
                  source_tail_trimmed=len(data) < volume.logical_bytes)
    return image, files, report
