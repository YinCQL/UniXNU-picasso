#!/usr/bin/env python3
"""Fetch and authenticate the stock touch firmware, or verify a local copy."""
import argparse
import json
from pathlib import Path
import urllib.request
import picasso_touch_firmware as firmware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        data = args.output.read_bytes()
    elif args.source:
        data = args.source.read_bytes()
    else:
        request = urllib.request.Request(firmware.URL, headers={'User-Agent': 'UniXNU-picasso'})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(firmware.SIZE + 1)
    result = firmware.verify(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists():
        with args.output.open('xb') as stream:
            stream.write(data)
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
