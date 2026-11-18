#!/usr/bin/env python3
"""Verify and stage the bundled factory touch firmware, or an explicit local copy."""
import argparse
import json
from pathlib import Path
import picasso_touch_firmware as firmware
from picasso_firmware import device_root, read_file, stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pin = firmware.PINS['files'][firmware.NAME]
    source = args.source or device_root() / 'firmware/novatek' / firmware.NAME
    data = read_file(source, pin)
    result = firmware.verify(data)
    stage(args.output.parent, {args.output.name: data}, {'files': {args.output.name: pin}})
    target = args.output.with_suffix('.json')
    if target == args.output or target.is_symlink():
        raise ValueError('Invalid touch firmware report path')
    target.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
