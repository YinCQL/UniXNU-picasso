#!/usr/bin/env python3
"""Check the original display/HID ABI inputs before SDK archive compilation."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--native-prelink-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.native_prelink_dir.resolve()))
    from picasso_original_abi import OriginalReference, qualify_legacy, qualify_hid

    root = Path(__file__).resolve().parents[1]
    board = tomllib.loads((root / 'board.toml').read_text(encoding='utf-8'))
    reference = OriginalReference(args.kernelcache, board)
    audit = qualify_legacy(reference)
    path = root / 'scripts/build-native-legacy-display.py'
    spec = importlib.util.spec_from_file_location('picasso_legacy_builder', path)
    legacy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(legacy)
    legacy.validate_original_code(reference, audit)
    hid = qualify_hid(reference)
    report = dict(passed=True, original_sha256=audit['original_sha256'], slide=audit['slide'],
                  legacy_virtual_entries=audit['legacy_virtual_entries'], hid_virtual_entries=len(hid),
                  sdk_compiled=False, runtime_qualified=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: pinned original display/HID ABI; no previous candidate required')


if __name__ == '__main__':
    main()
