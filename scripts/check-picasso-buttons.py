#!/usr/bin/env python3
"""Check production buttons, brightness and ARM64 bodies; no SDK/device qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract(text, marker):
    start = text.index(marker)
    cursor, depth = text.index('{', start) + 1, 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang++') or shutil.which('clang'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    hid = (ROOT / 'package/PicassoHidInput.cpp').read_text()
    consumer = (ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp').read_text()
    sink = hid[hid.index('static IOLock *buttonLock;'):hid.index('extern "C" void picasso_hid_physical_button(')]
    (out / 'button-sink.inc').write_text(sink)
    (out / 'button-consumer.inc').write_text('\n'.join(extract(consumer, n) for n in
        ('static bool buttonsNotify(', 'static IOReturn buttonsAction(', 'static IOReturn stopButtons(')))
    backlight = (ROOT / 'package/PicassoBacklight.cpp').read_text()
    (out / 'brightness-backend.inc').write_text('\n'.join(extract(backlight, n) for n in
        ('extern "C" IOReturn picasso_backlight_power(', 'extern "C" IOReturn picasso_backlight_level(',
         'extern "C" bool picasso_backlight_current(', 'extern "C" void picasso_backlight_shutdown(')))
    brightness = (ROOT / 'package/PicassoBrightness.cpp').read_text()
    brightness = re.sub(r'^#include <[^>]+>[^\n]*\n', '', brightness, flags=re.M)
    (out / 'brightness-service.inc').write_text(brightness)
    spmi = (ROOT / 'package/PicassoSpmi.cpp').read_text()
    buttons = (ROOT / 'package/PicassoButtons.cpp').read_text()
    assert 'picasso_spmi_read8(' in buttons and 'picasso_spmi_read8(' in backlight
    assert 'picasso_spmi_write8(' in backlight and 'picasso_spmi_init()' in buttons and 'picasso_spmi_init()' in backlight
    assert 'controllerLock' in spmi and spmi.count('IOSimpleLock *controllerLock;') == 1
    assert 'IOSimpleLockLockDisableInterrupt' in spmi and 'IOSimpleLockUnlockEnableInterrupt' in spmi
    assert 'va[offset' not in buttons and 'va[offset' not in backlight
    assert "ROOT / 'kits/picasso/PicassoSpmi.cpp'" in (ROOT / 'scripts/build-picasso-display.py').read_text()
    runs = []

    def run(name, cmd, execute=False):
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out / (name + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        runs.append(dict(name=name, command=cmd, passed=True, output=r.stdout))
        if execute:
            run(name + '-run', [cmd[cmd.index('-o') + 1]])

    for source in ('picasso-buttons-host.cpp', 'picasso-buttons-dispatch-host.cpp', 'picasso-backlight-host.cpp', 'picasso-brightness-host.cpp', 'picasso-spmi-host.cpp'):
        for sanitized in (False, True):
            name = Path(source).stem + ('-sanitized' if sanitized else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            cmd = [args.cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1',
                   '-I', str(ROOT / 'package'), '-I', str(out),
                   str(ROOT / 'scripts/tests' / source), '-o', str(exe)]
            if source == 'picasso-spmi-host.cpp' and os.name != 'nt':
                cmd += ['-pthread']
            if sanitized:
                cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            run(name, cmd, True)

    # Compile entire production worker/bridge bodies, including inline ARM64 MMIO
    # barriers. These explicit API declarations are not a substitute for XNU headers.
    for name in ('PicassoHidInput', 'PicassoButtons', 'PicassoBacklight', 'PicassoSpmi'):
        body = (ROOT / 'package' / (name + '.cpp')).read_text()
        body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', body, flags=re.M)
        source = out / (name + '.cpp')
        source.write_text('#include "picasso-input-kernel-api.h"\n' + body)
        run(name + '-arm64', [args.cc, '-target', 'arm64-apple-ios17.0', '-std=c++17',
            '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
            '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-I', str(ROOT / 'scripts/tests'), '-I', str(ROOT / 'package'),
            '-I', str(ROOT / 'drivers/NativeNovatekTouch'), '-c', str(source), '-o', str(out / (name + '.o'))])
    source = out / 'PicassoBrightness.cpp'
    source.write_text('#define PICASSO_BRIGHTNESS_ARM_API\n#include "picasso-brightness-api.h"\n' + brightness)
    run('PicassoBrightness-arm64', [args.cc, '-target', 'arm64-apple-ios17.0', '-std=c++17',
        '-fapple-kext', '-fno-c++-static-destructors',
        '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-mgeneral-regs-only',
        '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'scripts/tests'), '-I', str(ROOT / 'package'),
        '-c', str(source), '-o', str(out / 'PicassoBrightness.o')])
    inputs = [ROOT / 'package' / n for n in ('PicassoButtonsCore.h', 'PicassoButtons.cpp', 'PicassoHidInput.cpp', 'PicassoBacklightCore.h', 'PicassoBacklight.cpp', 'PicassoBrightness.cpp', 'PicassoSpmi.cpp', 'PicassoSpmi.h', 'PicassoSpmiCore.h')]
    inputs += [ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp', Path(__file__)]
    inputs += [ROOT / 'scripts/build-picasso-display.py', ROOT / 'scripts/build-picasso-hid.py']
    inputs += [ROOT / 'scripts/tests' / n for n in ('picasso-backlight-host.cpp',
        'picasso-brightness-host.cpp', 'picasso-brightness-api.h', 'picasso-input-kernel-api.h', 'picasso-spmi-host.cpp')]
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        hardware_tested=False, kernel_sdk_compiled=False,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}), indent=2) + '\n')
    print('PASS: PMIC protocol, button dispatch lifecycle, sanitizer tests and production ARM64 bodies')


if __name__ == '__main__':
    main()
