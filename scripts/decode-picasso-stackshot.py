#!/usr/bin/env python3
"""Recover a complete, CRC-checked panic stackshot from a saved CDC log.

Reads local files only. Optionally use the matching XNU kcdata.py to decode the
validated bytes. An incomplete latest capture is an error, never a reason to
silently use an older successful capture. All output goes to a new directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

MAX_BYTES = 512 * 1024
MAX_EXPANDED = 16 * 1024 * 1024
PREFIX = b'PICASSO_STACKSHOT '
BEGIN = re.compile(rb'begin v=1 bytes=([0-9]+) encoding=hex chunk=96 crc=crc32-ieee')
DATA = re.compile(rb'data offset=([0-9a-f]{8}) hex=([0-9a-f]+)')
END = re.compile(rb'end bytes=([0-9]+) crc32=([0-9a-f]{8})')
STACKSHOT = 0x59a25807
COMPRESSED = 0x434f4d50
BUFFER_END = 0xf19158ed
STRUCT_HAS_PADDING = 0x80


def extract(log, index=-1):
    sessions = []
    current = None
    for lineno, line in enumerate(log.split(b'\n'), 1):
        line = line.rstrip(b'\r')
        if not line.startswith(PREFIX):
            continue
        message = line[len(PREFIX):]
        if message.startswith((b'begin ', b'skipped ')) or current is None:
            current = dict(line=lineno, data=bytearray(), error=None, end=False)
            sessions.append(current)
            match = BEGIN.fullmatch(message)
            if not match or not 0 < int(match[1]) <= MAX_BYTES:
                current['error'] = f'invalid/missing begin or skipped export at line {lineno}'
                continue
            current['size'] = int(match[1])
            continue
        if current['error']:
            continue
        if current['end']:
            current['error'] = f'unexpected record after end at line {lineno}'
            continue
        match = DATA.fullmatch(message)
        if match:
            encoded = match[2]
            remaining = current['size'] - len(current['data'])
            expected = min(96, remaining)
            if int(match[1], 16) != len(current['data']) or expected <= 0 or len(encoded) != 2 * expected:
                current['error'] = f'chunk gap, duplicate, order or length error at line {lineno}'
            else:
                current['data'].extend(bytes.fromhex(encoded.decode('ascii')))
            continue
        match = END.fullmatch(message)
        if match:
            blob = current['data']
            if int(match[1]) != current['size'] or len(blob) != current['size']:
                current['error'] = f'footer length mismatch at line {lineno}'
            elif int(match[2], 16) != zlib.crc32(blob):
                current['error'] = f'CRC mismatch at line {lineno}'
            else:
                current.update(end=True, end_line=lineno, crc32=match[2].decode())
            continue
        current['error'] = f'aborted or malformed export at line {lineno}'
    if not sessions:
        raise ValueError('No PICASSO_STACKSHOT export; a Stackshot Succeeded summary contains no payload')
    try:
        current = sessions[index]
    except IndexError as exc:
        raise ValueError(f'Capture index {index} outside {len(sessions)} exports') from exc
    if current['error']:
        raise ValueError(current['error'])
    if not current['end']:
        raise ValueError(f'Incomplete export beginning at line {current["line"]}; no validated footer')
    return bytes(current['data']), {k: v for k, v in current.items() if k != 'data'} | {'captures': len(sessions), 'index': index}


def unpack_kcdata(blob):
    """Bound the compression envelope before handing it to XNU's decoder.

    Layout matches kern_cdata.c and KCCompressedBufferObject in the pinned XNU.
    This is envelope validation, not a replacement for its typed kcdata parser.
    """
    if not 32 <= len(blob) <= MAX_BYTES:
        raise ValueError('Truncated or oversized kcdata buffer')
    magic, size, flags = struct.unpack_from('<IIQ', blob)
    # kcdata_get_memory_addr sets HAS_PADDING even for a zero-length root.
    # XNU's decompressor also emits a synthetic root with flags=0.
    if size or flags not in (0, STRUCT_HAS_PADDING):
        raise ValueError('Unexpected kcdata root header')
    # finalize_kcdata writes this uncompressed trailer after finalizing zlib.
    if blob[-16:] != struct.pack('<IIQ', BUFFER_END, 0, 0):
        raise ValueError('Missing kcdata buffer end marker')
    if magic == STACKSHOT:
        return blob
    if magic != COMPRESSED:
        raise ValueError(f'Unsupported kcdata magic {magic:#x}')
    position = 16
    header = {}
    for _ in range(4):
        if position + 16 > len(blob):
            raise ValueError('Truncated compression metadata')
        kind, size, flags = struct.unpack_from('<IIQ', blob, position)
        position += 16
        if kind == STACKSHOT and size == 0 and flags in (0, STRUCT_HAS_PADDING):
            break
        if kind != 3 or size != 48 or flags not in (0, STRUCT_HAS_PADDING | 8) or position + size > len(blob):
            raise ValueError('Invalid compression descriptor')
        name = blob[position:position + 32].split(b'\0', 1)[0]
        if name in header:
            raise ValueError('Duplicate compression descriptor')
        header[name] = struct.unpack_from('<Q', blob, position + 32)[0]
        position += size
    else:
        raise ValueError('Missing inner stackshot header')
    if set(header) != {b'kcd_c_type', b'kcd_c_totalin', b'kcd_c_totalout'} or header[b'kcd_c_type'] != 1:
        raise ValueError('Unsupported compression metadata')
    packed = header[b'kcd_c_totalout']
    expanded = header[b'kcd_c_totalin']
    if not 0 < packed <= MAX_BYTES or position + packed + 16 != len(blob) or not 0 < expanded <= MAX_EXPANDED:
        raise ValueError('Compression lengths exceed bounds')
    decoder = zlib.decompressobj()
    data = decoder.decompress(blob[position:position + packed], MAX_EXPANDED + 1)
    if len(data) != expanded or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('Incomplete, oversized or mismatched compressed stream')
    return (struct.pack('<IIQ', STACKSHOT, 0, 0) + data + bytes(-len(data) % 16)
            + struct.pack('<IIQ', BUFFER_END, 0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True, help='Saved log or shared-read snapshot')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--index', type=int, default=-1, help='Export index; default selects the latest, even if incomplete')
    parser.add_argument('--kcdata-script', type=Path, help='Matching XNU tools/lldbmacros/kcdata.py')
    args = parser.parse_args()
    with args.log.open('rb') as stream:
        raw = stream.read(64 * 1024 * 1024 + 1)
    if len(raw) > 64 * 1024 * 1024:
        raise ValueError('Log exceeds 64 MiB; select the relevant saved session first')
    blob, metadata = extract(raw, args.index)
    unpacked = unpack_kcdata(blob)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'stackshot.kcdata').write_bytes(blob)
    (out / 'stackshot.uncompressed.kcdata').write_bytes(unpacked)
    metadata.update(log_sha256=hashlib.sha256(raw).hexdigest(), bytes=len(blob),
                    sha256=hashlib.sha256(blob).hexdigest(), uncompressed_bytes=len(unpacked))
    (out / 'transport.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    if args.kcdata_script:
        decoder = args.kcdata_script.resolve()
        result = subprocess.run([sys.executable, '-X', 'utf8', str(decoder),
                                 str(out / 'stackshot.uncompressed.kcdata')],
                                capture_output=True, text=True, encoding='utf-8', timeout=60)
        (out / 'decoder.stdout.txt').write_text(result.stdout, encoding='utf-8')
        (out / 'decoder.stderr.txt').write_text(result.stderr, encoding='utf-8')
        if result.returncode:
            raise ValueError('XNU kcdata decoder failed; raw capture and diagnostics are preserved')
        obj = json.loads(result.stdout)
        if 'kcdata_stackshot' not in obj:
            raise ValueError('Decoder returned no stackshot object')
        (out / 'stackshot.json').write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')
        metadata['decoder_sha256'] = hashlib.sha256(decoder.read_bytes()).hexdigest()
        (out / 'transport.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print(f'Validated {len(blob)} stackshot bytes; saved to {out}')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, zlib.error, subprocess.TimeoutExpired) as exc:
        print(f'error: {exc}', file=sys.stderr)
        sys.exit(1)
