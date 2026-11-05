#!/usr/bin/env python3
"""Compile and ad-hoc sign the SpringBoard inject library for this board (macOS CI).

  build-picasso-sb-inject.py SOURCE.m OUTPUT.dylib

Builds an arm64 iOS dynamic library with the runner's iPhoneOS SDK and ad-hoc
signs it (codesign -s -). prepare-picasso-ui-trust.py qualifies its signed code
pages and adds its exact CDHash to the owner trust module. The kernel inserts
it into SpringBoard (boot-arg picasso_inject_sb).

SOURCE is the library's Objective-C entry point. The default source includes
the Picasso status bar metric and the version-qualified ASTC preflight.
picasso_inject_apps=1 also inserts it into
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
                   '-dynamiclib', '-Wl,-install_name,/private/var/picasso/libpicasso-sb.dylib', '-fobjc-arc', '-framework', 'Foundation',
                   '-framework', 'UIKit', '-framework', 'Metal', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-o', str(output), str(source)]
    subprocess.run(compile_cmd, check=True)
    # The payload's scoped UI trust module must match this final signature.
    # This owned library runs on the native-4K kernel. Sign 4K code slots
    # directly, avoiding the 16K-to-4K preflight/read/allocation path after
    # AMFI registration. Segment alignment and all signature checks remain.
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', str(output)], check=True)
    from picasso_ui_trust import library_identity
    if library_identity(output.read_bytes())['code_page_bytes'] != 4096:
        raise RuntimeError('UI library must be signed with native 4K code slots')
    identity = subprocess.check_output(['codesign', '-dvvv', str(output)], stderr=subprocess.STDOUT).decode()
    print(f'picasso: built {output.name} ({output.stat().st_size} bytes)')
    for key in ('CodeDirectory', 'Signature', 'CDHash'):
        for probe_line in identity.splitlines():
            if probe_line.startswith(key):
                print('  ' + probe_line)
                break


if __name__ == '__main__':
    main()
