#!/usr/bin/env python3
"""Convert raw images to Android sparse RAW/FILL chunks; no device operations.

Format: AOSP system/core/libsparse/sparse_format.h. DONT_CARE is 0xCAC3;
0xCAC4 carries a CRC32. The writer uses zero FILL, never DONT_CARE: flashing
DONT_CARE does not guarantee erased/zero disk contents. Partial blocks are padded.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import zlib

BLK = 4096
BUFFER = 8 * 1024 * 1024
SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
HEADER = struct.Struct("<IHHHHIIII")
CHUNK = struct.Struct("<HHII")


def exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("truncated sparse image")
    return data


def convert(src, dst):
    src, dst = Path(src), Path(dst)
    if src.resolve() == dst.resolve() or (dst.exists() and os.path.samefile(src, dst)):
        raise ValueError("source and output must be different files")
    temporary = None
    try:
        with src.open("rb") as source:
            before = os.fstat(source.fileno())
            blocks = (before.st_size + BLK - 1) // BLK
            if blocks > 0xffffffff:
                raise ValueError("raw image exceeds sparse block count")
            with tempfile.NamedTemporaryFile(dir=dst.parent, prefix=dst.name + ".", delete=False) as out:
                temporary = Path(out.name)
                out.write(bytes(HEADER.size))
                count, consumed = 0, 0
                while data := source.read(BUFFER):
                    consumed += len(data)
                    data += bytes((-len(data)) % BLK)
                    size = len(data)
                    if not any(data):
                        out.write(CHUNK.pack(FILL, 0, size // BLK, CHUNK.size + 4))
                        out.write(bytes(4))
                    else:
                        out.write(CHUNK.pack(RAW, 0, size // BLK, CHUNK.size + size))
                        out.write(data)
                    count += 1
                after = os.fstat(source.fileno())
                if consumed != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                    raise ValueError("source changed during conversion")
                out.seek(0)
                out.write(HEADER.pack(SPARSE_MAGIC, 1, 0, HEADER.size, CHUNK.size, BLK, blocks, count, 0))
                out.flush()
                os.fsync(out.fileno())
        os.replace(temporary, dst)
        temporary = None
        return count
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def expanded(stream):
    magic, major, _, file_hdr, chunk_hdr, blk, blocks, chunks, checksum = HEADER.unpack(exact(stream, HEADER.size))
    if magic != SPARSE_MAGIC or major != 1 or not HEADER.size <= file_hdr <= 65535 or not CHUNK.size <= chunk_hdr <= 65535 or not blk or blk % 4:
        raise ValueError("bad sparse header")
    exact(stream, file_hdr - HEADER.size)
    used, crc = 0, 0
    for _ in range(chunks):
        kind, _, count, total = CHUNK.unpack(exact(stream, CHUNK.size))
        exact(stream, chunk_hdr - CHUNK.size)
        size = count * blk
        if used + count > blocks:
            raise ValueError("chunk exceeds declared image size")
        if kind == CRC32:
            if count or total != chunk_hdr + 4:
                raise ValueError("bad CRC32 chunk")
            if struct.unpack("<I", exact(stream, 4))[0] != crc:
                raise ValueError("CRC32 mismatch")
            continue
        if not count:
            raise ValueError("empty data chunk")
        if kind == RAW:
            if total != chunk_hdr + size:
                raise ValueError("bad RAW chunk length")
            pattern = None
        elif kind == FILL:
            if total != chunk_hdr + 4:
                raise ValueError("bad FILL chunk length")
            pattern = exact(stream, 4)
        elif kind == DONT_CARE:
            if total != chunk_hdr:
                raise ValueError("bad DONT_CARE chunk length")
            pattern = bytes(4)
        else:
            raise ValueError("unknown sparse chunk type")
        remaining = size
        while remaining:
            take = min(BUFFER, remaining)
            data = exact(stream, take) if pattern is None else pattern * (take // 4)
            crc = zlib.crc32(data, crc)
            yield data
            remaining -= take
        used += count
    if used != blocks or stream.read(1):
        raise ValueError("block count mismatch or trailing bytes")
    if checksum and checksum != crc:
        raise ValueError("header CRC32 mismatch")


def check(path, raw=None):
    digest = hashlib.sha256()
    total = 0
    reference = Path(raw).open("rb") if raw is not None else None
    try:
        length = os.fstat(reference.fileno()).st_size if reference else 0
        with Path(path).open("rb") as stream:
            header = HEADER.unpack(exact(stream, HEADER.size))
            if not header[5] or (reference and (length + header[5] - 1) // header[5] != header[6]):
                raise ValueError("raw length differs from expanded image")
            stream.seek(0)
            for data in expanded(stream):
                if reference:
                    n = min(len(data), max(0, length - total))
                    expected = exact(reference, n) + bytes(len(data) - n)
                    if data != expected:
                        raise ValueError("expansion differs from raw image or zero padding")
                total += len(data)
                digest.update(data)
        return dict(chunks=header[7], blocks=header[6], expanded_bytes=total,
                    expanded_sha256=digest.hexdigest(), identical=True if reference else None)
    finally:
        if reference:
            reference.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.output:
        print("converted chunks:", convert(args.source, args.output))
        if args.check:
            print(check(args.output, args.source))
    else:
        print(check(args.source))


if __name__ == "__main__":
    main()
