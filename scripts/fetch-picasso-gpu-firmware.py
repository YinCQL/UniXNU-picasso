#!/usr/bin/env python3
"""Fetch exact stock A620 firmware, or validate and stage the local stock set."""
import argparse
import json
from pathlib import Path
import urllib.request
import picasso_gpu_firmware as firmware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument('--source', type=Path)
    inputs.add_argument('--archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        report = firmware.verify_directory(args.output)
    else:
        if args.source:
            firmware.verify_directory(args.source)
            contents = {name: (args.source / name).read_bytes() for name in firmware.FILES}
        else:
            if args.archive:
                data = args.archive.read_bytes()
            else:
                request = urllib.request.Request(firmware.URL, headers={'User-Agent': 'UniXNU-picasso'})
                with urllib.request.urlopen(request, timeout=60) as response:
                    data = response.read(firmware.ARCHIVE_BYTES + 1)
            contents = firmware.unpack(data)
        # All inputs have passed before creating a destination. Recheck local
        # bytes after the second read to reject a concurrent source mutation.
        for name, blob in contents.items():
            firmware.verify(name, blob)
        args.output.mkdir(parents=True, exist_ok=False)
        for name, blob in contents.items():
            with (args.output / name).open('xb') as stream:
                stream.write(blob)
        report = firmware.verify_directory(args.output)
    (args.output / 'firmware.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: eight exact stock Adreno 620 firmware files staged; hardware validation pending')


if __name__ == '__main__':
    main()
