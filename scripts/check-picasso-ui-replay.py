#!/usr/bin/env python3
"""Cold-replay UI/input-related XNU files, then run host fixtures. No kernel compilation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
FILES = ('bsd/kern/kern_exec.c', 'bsd/kern/kern_mman.c', 'bsd/kern/kern_descrip.c',
         'bsd/kern/kern_newsysctl.c', 'security/mac_file.c', 'security/mac_internal.h',
         'security/mac_base.c', 'iokit/Kernel/IOUserClient.cpp', 'bsd/kern/kern_exit.c',
         'bsd/dev/picasso_usb.c', 'iokit/Kernel/IONVRAM.cpp', 'iokit/Kernel/IONVRAMCHRPHandler.cpp',
         'iokit/IOKit/IONVRAM.h')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True, help='unixnu --prepare tree')
    parser.add_argument('--reference', type=Path, help='Local XNU Git repository; otherwise download pinned raw files')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    baseline = (ROOT / 'scripts/build-xnu-4k-baseline.sh').read_text()
    rev = re.search(r'XNU_COMMIT=\$\{UNIXNU_XNU_COMMIT:-([0-9a-f]{40})\}', baseline)[1]
    xnu = out / 'xnu'
    original = {}
    for name in FILES:
        if name == 'bsd/dev/picasso_usb.c': continue  # Added by the board overlay.
        if args.reference:
            data = subprocess.check_output(['git', '-c', 'core.autocrlf=false', '-C', str(args.reference), 'show', rev + ':' + name])
        else:
            with urllib.request.urlopen(f'https://raw.githubusercontent.com/apple-oss-distributions/xnu/{rev}/{name}', timeout=60) as response:
                data = response.read()
        target = xnu / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        original[name] = hashlib.sha256(data).hexdigest()
    # Isolate git apply even when --output is nested inside the device checkout.
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'init', '-q', str(xnu)], check=True)
    applied = []
    patches = sorted((args.tree / 'patches/xnu').glob('*.patch'))
    if not any('picasso-ui-runtime' in p.name for p in patches):
        raise RuntimeError('Prepared tree is missing current UI overlay')
    for patch in patches:
        raw = patch.read_text().replace('\r\n', '\n')
        if not any('+++ b/' + name in raw for name in FILES):
            continue
        subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '--unsafe-paths',
                        *['--include=' + name for name in FILES], '-'], input=raw.encode(), cwd=xnu, check=True)
        applied.append(patch.name)
    results = dict(scope=__doc__, revision=rev, original=original, patches=applied,
                   replayed={name: hashlib.sha256((xnu / name).read_bytes()).hexdigest() for name in FILES})
    (out / 'replay.json').write_text(json.dumps(results, indent=2) + '\n')
    for script, label in [('check-picasso-ui-runtime.py', 'runtime'), ('check-picasso-ios16-surface-abi.py', 'surface'), ('check-picasso-nvram.py', 'nvram')]:
        subprocess.run([sys.executable, '-X', 'utf8', '-B', str(ROOT / 'scripts' / script), '--xnu', str(xnu), '--output', str(out / label)], check=True)
    print('PASS pinned XNU cold replay, UI library/MAC helpers and display dispatch')


if __name__ == '__main__':
    main()
