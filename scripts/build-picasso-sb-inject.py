#!/usr/bin/env python3
"""Compile and ad-hoc sign the SpringBoard inject library for this board (macOS CI).

  build-picasso-sb-inject.py SOURCE.m OUTPUT.dylib

Builds an arm64 iOS dynamic library with the runner's iPhoneOS SDK and ad-hoc
signs it (codesign -s -) so dyld will map it. The kernel inserts it into
SpringBoard (boot-arg picasso_inject_sb); AMFI accepts the ad-hoc signature only
because the board also boots amfi_allow_any_signature=1.

SOURCE is the library's single Objective-C source. The default source supplies
the Picasso status bar metric. picasso_inject_apps=1 also inserts it into
application bundle executables, so app and SpringBoard metrics agree.
"""
import subprocess
import sys
from pathlib import Path


def xcrun(*args):
    return subprocess.check_output(['xcrun', '--sdk', 'iphoneos', *args]).decode().strip()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    sdk = xcrun('--show-sdk-path')
    clang = xcrun('-find', 'clang')
    output.parent.mkdir(parents=True, exist_ok=True)
    compile_cmd = [clang, '-arch', 'arm64', '-isysroot', sdk, '-miphoneos-version-min=16.0',
                   '-dynamiclib', '-fobjc-arc', '-framework', 'Foundation',
                   '-framework', 'UIKit', '-O2',
                   '-o', str(output), str(source)]
    subprocess.run(compile_cmd, check=True)
    # Ad-hoc signature: the bytes AMFI needs present; amfi_allow_any_signature skips validating them.
    subprocess.run(['codesign', '--force', '--sign', '-', str(output)], check=True)
    identity = subprocess.check_output(['codesign', '-dvvv', str(output)], stderr=subprocess.STDOUT).decode()
    print(f'picasso: built {output.name} ({output.stat().st_size} bytes)')
    for key in ('CodeDirectory', 'Signature', 'CDHash'):
        for probe_line in identity.splitlines():
            if probe_line.startswith(key):
                print('  ' + probe_line)
                break


if __name__ == '__main__':
    main()
