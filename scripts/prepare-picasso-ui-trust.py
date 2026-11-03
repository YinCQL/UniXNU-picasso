#!/usr/bin/env python3
"""Add exactly the compiled status bar library to the original trust bundle."""
import argparse
import json
from pathlib import Path
from picasso_ui_trust import make_bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    bundle, report = make_bundle(args.original.read_bytes(), args.library.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: unchanged Apple modules plus one exact UI library CDHash', report['library']['cdhash'])


if __name__ == '__main__':
    main()
