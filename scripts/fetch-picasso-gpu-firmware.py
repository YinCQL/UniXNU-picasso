#!/usr/bin/env python3
"""Verify and stage bundled factory A620 firmware, or an explicit local input."""
import argparse
import json
from pathlib import Path
import picasso_gpu_firmware as firmware
from picasso_firmware import device_root, read_group, stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument('--source', type=Path)
    inputs.add_argument('--archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.archive:
        if args.archive.is_symlink() or not args.archive.is_file() or args.archive.stat().st_size != firmware.ARCHIVE_BYTES:
            raise ValueError('Invalid GPU firmware archive')
        contents = firmware.unpack(args.archive.read_bytes())
    else:
        contents = read_group('adreno620', args.source or device_root() / 'firmware/adreno620')
    stage(args.output, contents, firmware.PINS)
    report = firmware.verify_directory(args.output)
    target = args.output / 'firmware.json'
    if target.is_symlink():
        raise ValueError('Invalid GPU firmware report path')
    target.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: eight exact factory Adreno 620 files staged; hardware validation pending')


if __name__ == '__main__':
    main()
