#!/usr/bin/env python3
"""Exercise the touch protocol and actual producer handoff with host API models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import picasso_touch_firmware as firmware

ROOT = Path(__file__).resolve().parents[1]


def extract(text, marker):
    start = text.index(marker)
    cursor = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang') or shutil.which('cc'))
    parser.add_argument('--firmware', type=Path)
    args = parser.parse_args()
    if not args.cc:
        parser.error('An installed C/C++ compiler is required')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'drivers/NativeNovatekTouch'
    probe = (source / 'TouchProbe.cpp').read_text()
    hid = (ROOT / 'package/PicassoHidInput.cpp').read_text()
    bodies = '\n'.join(extract(hid, marker) for marker in (
        'extern "C" void native_touch_notify(', 'extern "C" void picasso_hid_physical_contact(',
        'static void notifyPublished(', 'static void publishContact('))
    bodies += '\n' + extract(probe, 'static void notifyThunk(') + '\n' + extract(probe, 'bool publish(')
    (out / 'producer-bodies.h').write_text(bodies)
    report_bodies = '\n'.join(extract(probe, marker) for marker in (
        'bool parseReport(', 'bool sameFingerSet(', 'int sampleContacts('))
    (out / 'report-bodies.h').write_text(report_bodies)
    runs = []
    for asan in (False, True):
        for label, language, files in (
            ('protocol', 'c', [source / 'Test/main.c', source / 'NvtProtocol.c', source / 'NvtTouchCore.c']),
            ('handoff', 'c++', [ROOT / 'scripts/tests/picasso-touch-handoff-host.cpp']),
            ('report', 'c++', [ROOT / 'scripts/tests/picasso-touch-report-host.cpp']),
        ):
            name = label + ('-asan' if asan else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            command = [args.cc, '-x', language, '-std=c11' if language == 'c' else '-std=c++17',
                       '-O0', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-ignored-attributes',
                       '-I', str(source), '-I', str(out), *map(str, files), '-o', str(exe)]
            if os.name != 'nt':
                command.append('-lm' if language == 'c' else
                               '-lc++' if sys.platform == 'darwin' else '-lstdc++')
            if asan:
                command += ['-fsanitize=address', '-fno-omit-frame-pointer']
            result = subprocess.run(command, capture_output=True, text=True)
            (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stderr)
            invocation = [str(exe)]
            if label == 'protocol' and args.firmware:
                firmware.verify(args.firmware.read_bytes())
                invocation.append(str(args.firmware.resolve()))
            result = subprocess.run(invocation, capture_output=True, text=True)
            (out / (name + '-run.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            runs.append(dict(name=name, passed=True, command=command, output=result.stdout.strip()))
    result = dict(scope=__doc__, runs=runs, passed=True, hardware_tested=False,
                  report_bodies_sha256=hashlib.sha256(report_bodies.encode()).hexdigest(),
                  producer_bodies_sha256=hashlib.sha256(bodies.encode()).hexdigest())
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: touch protocol and production producer handoff, normal and ASan')


if __name__ == '__main__':
    main()
