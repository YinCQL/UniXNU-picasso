#!/usr/bin/env python3
"""Host-test the production software AES service with Windows CNG and IOKit stand-ins."""
import argparse
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'drivers/NativeAES/NativeAES.cpp').read_text(encoding='utf-8')
    production = re.sub(r'^#(?:include|pragma).*\n', '', source, flags=re.M)
    fixture = (ROOT / 'scripts/tests/picasso-aes-host.cpp').read_text(encoding='utf-8')
    spec = importlib.util.spec_from_file_location('runner', ROOT / 'scripts/check-picasso-boot-state.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.run_host(out, source, production, fixture, 'aes-production.inc', 12)
    report = json.loads((out / 'results.json').read_text(encoding='utf-8'))
    report['scope'] = 'Production AES code; real Windows CNG CBC and host IOKit descriptors. Not Apple corecrypto/SDK or hardware.'
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
