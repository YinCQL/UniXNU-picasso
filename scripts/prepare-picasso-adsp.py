#!/usr/bin/env python3
"""Extract and validate the supplied stock NON-HLOS.bin into local ADSP PAS inputs."""
import argparse
import json
from pathlib import Path
from picasso_adsp_firmware import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image, files, report = prepare(args.source)
    args.output.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        (args.output / name).write_bytes(data)
    (args.output / 'adsp.mbn').write_bytes(image)
    (args.output / 'firmware.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'PASS: {len(files)} stock inputs, {len(image)}-byte ADSP image; PAS authentication not run')


if __name__ == '__main__':
    main()
