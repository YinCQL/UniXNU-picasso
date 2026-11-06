#!/usr/bin/env python3
"""Convert a raw disk image to the Android sparse format for `fastboot flash`.

Why this exists: a userdata image larger than the partition check in
fastboot 33+ trips `copy_avb_footer()`, which reads the whole image into memory
and fails with "Failed reading from userdata" on the 8.4 GiB iOS 16 image.
libsparse returns early for a sparse buffer, so a spec-compliant sparse image
flashes with the stock fastboot.

Layout (little endian):

    header (28 bytes)
        u32 magic 0xED26FF3A, u16 major 1, u16 minor 0,
        u16 file_hdr_sz 28, u16 chunk_hdr_sz 12,
        u32 blk_sz, u32 total_blks, u32 total_chunks, u32 image_checksum

    chunk header (12 bytes)
        u16 type, u16 reserved, u32 chunk_sz, u32 total_sz

      RAW  (0xCAC1) total_sz = 12 + chunk_sz*blk_sz, then the data
      FILL (0xCAC2) total_sz = 12 + 4, then the u32 fill pattern
      DONT_CARE (0xCAC4) total_sz = 12

libsparse validates total_sz for RAW and FILL and rejects the file when either
is wrong, so both are written exactly. `--check` re-reads the result with a
strict parser and, with a raw file, also proves the expansion is byte-identical.
"""
import argparse
import hashlib
import struct
import sys

BLK = 4096
CHUNK_HDR_SZ = 12
FILE_HDR_SZ = 28
SPARSE_MAGIC = 0xED26FF3A
MAX_CHUNK_BYTES = 256 * 1024 * 1024
ZERO = b"\0" * BLK


def convert(src, dst):
    total = src.stat().st_size
    chunks = []
    with src.open("rb") as source:
        # Pass 1: measure the runs so the header can be written first.
        while True:
            block = source.read(BLK)
            if not block:
                break
            is_fill = block == ZERO
            count = 1
            while count * BLK < MAX_CHUNK_BYTES:
                nxt = source.read(BLK)
                if not nxt:
                    break
                if (nxt == ZERO) != is_fill or (is_fill and len(nxt) != BLK):
                    source.seek(-len(nxt), 1)
                    break
                count += 1
            chunks.append((is_fill, count))
    with src.open("rb") as source, dst.open("wb") as out:
        out.write(struct.pack("<IHHHHIIII", SPARSE_MAGIC, 1, 0, FILE_HDR_SZ,
                              CHUNK_HDR_SZ, BLK, (total + BLK - 1) // BLK,
                              len(chunks), 0))
        for is_fill, count in chunks:
            if is_fill:
                # A FILL chunk carries no source bytes, so step the source over it.
                source.seek(count * BLK, 1)
                out.write(struct.pack("<HHII", 0xCAC2, 0, count, CHUNK_HDR_SZ + 4))
                out.write(struct.pack("<I", 0))
                continue
            size = count * BLK
            out.write(struct.pack("<HHII", 0xCAC1, 0, count, CHUNK_HDR_SZ + size))
            # The image expands to whole blocks: a final partial block is padded.
            data = source.read(size)
            if len(data) < size:
                data += b"\0" * (size - len(data))
            out.write(data)
    return len(chunks)


def check(path, raw=None):
    with path.open("rb") as stream:
        magic, major, minor, file_hdr, chunk_hdr, blk, total_blks, total_chunks, _ = \
            struct.unpack("<IHHHHIIII", stream.read(28))
        if (magic, major, file_hdr, chunk_hdr, blk) != (SPARSE_MAGIC, 1, FILE_HDR_SZ, CHUNK_HDR_SZ, BLK):
            raise SystemExit("bad sparse header")
        digest = hashlib.sha256()
        expanded = 0
        seen = 0
        while seen < total_chunks:
            chunk_type, _, chunk_sz, total_sz = struct.unpack("<HHII", stream.read(CHUNK_HDR_SZ))
            seen += 1
            if chunk_type == 0xCAC1:
                if total_sz != CHUNK_HDR_SZ + chunk_sz * blk:
                    raise SystemExit("RAW chunk %d has total_sz %d" % (seen, total_sz))
                remaining = chunk_sz * blk
                while remaining:
                    data = stream.read(min(remaining, 8 * 1024 * 1024))
                    if not data:
                        raise SystemExit("truncated RAW chunk %d" % seen)
                    digest.update(data)
                    remaining -= len(data)
            elif chunk_type == 0xCAC2:
                if total_sz != CHUNK_HDR_SZ + 4:
                    raise SystemExit("FILL chunk %d has total_sz %d" % (seen, total_sz))
                pattern = stream.read(4)
                digest.update(pattern * ((chunk_sz * blk) // 4))
            elif chunk_type == 0xCAC4:
                if total_sz != CHUNK_HDR_SZ:
                    raise SystemExit("DONT_CARE chunk %d has total_sz %d" % (seen, total_sz))
                digest.update(b"\0" * (chunk_sz * blk))
            else:
                raise SystemExit("unknown chunk type %#x at %d" % (chunk_type, seen))
            expanded += chunk_sz
        if expanded != total_blks:
            raise SystemExit("chunk blocks %d != header total_blks %d" % (expanded, total_blks))
        trailing = stream.read()
        if trailing:
            raise SystemExit("file has %d trailing bytes" % len(trailing))
    report = {"chunks": seen, "blocks": total_blks, "expanded_bytes": expanded * blk,
              "expanded_sha256": digest.hexdigest()}
    if raw is not None:
        # The expansion is block aligned; compare only the raw file's own length.
        length = raw.stat().st_size
        head = hashlib.sha256()
        with path.open("rb") as stream:
            stream.read(28)
            remaining = length
            while remaining > 0:
                chunk_type, _, chunk_sz, total_sz = struct.unpack("<HHII", stream.read(CHUNK_HDR_SZ))
                take = min(chunk_sz * blk, remaining)
                if chunk_type == 0xCAC1:
                    head.update(stream.read(take))
                    stream.seek(chunk_sz * blk - take, 1)
                elif chunk_type == 0xCAC2:
                    head.update(stream.read(4) * (take // 4))
                    stream.seek(chunk_sz * blk - take, 1)
                else:
                    head.update(b"\0" * take)
                remaining -= take
        with raw.open("rb") as stream:
            reference = hashlib.file_digest(stream, "sha256").hexdigest()
        report["raw_sha256"] = reference
        report["identical"] = reference == head.hexdigest()
        if not report["identical"]:
            raise SystemExit("expansion differs from the raw image")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", help="raw image (or the sparse image with --check without --output)")
    parser.add_argument("output", nargs="?", help="sparse image to write")
    parser.add_argument("--check", action="store_true",
                        help="re-read the output with a strict parser and prove it expands to the source")
    args = parser.parse_args()
    if not args.output:
        report = check(__import__("pathlib").Path(args.source))
        print("sparse ok: %(chunks)d chunks, %(blocks)d blocks, %(expanded_bytes)d bytes" % report)
        return
    from pathlib import Path
    source, output = Path(args.source), Path(args.output)
    chunks = convert(source, output)
    print("%s -> %s: %d bytes, %d chunks" % (source, output, output.stat().st_size, chunks))
    if args.check:
        report = check(output, source)
        print("check: expands to %(expanded_bytes)d bytes, sha256 %(expanded_sha256)s" % report)
        print("check: identical to the raw image" if report["identical"] else "check: DIFFERENT")


if __name__ == "__main__":
    sys.exit(main())
