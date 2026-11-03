#!/usr/bin/env python3
"""Build a PR13 experiment from the current iOS16 kernel and Data seed.

Usage: scripts/build-wf-payload.sh TAG ADDON [extra kernel args...]
Inputs: DATA_RAMDISK, FD, OWNER_TRUST, UNIXNU_KERNELCACHE, UNIXNU_UEFI,
UNIXNU_EDK2, UNIQUE_CHIP_ID. Optional memory overrides: OWNED_MEMORY, DRAM_SIZE.
GRID=0 omits sheng-display-grid=10. No kernel or firmware rebuild is performed.
"""
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def input_path(variable, default=None):
    value = os.environ.get(variable) or default
    if not value:
        raise ValueError(f'Set {variable} to the required local input')
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    if not path.is_file():
        raise ValueError(f'{variable} input missing: {path}')
    return path.resolve()


def main():
    if len(sys.argv) < 3 or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', sys.argv[1]):
        raise ValueError('Usage: build-wf-payload.sh TAG ADDON [extra kernel args...]')
    tag, addon_name, *extra = sys.argv[1:]
    kernel = input_path('WF_KERNEL', 'out/picasso/kernel/kernel.macho')
    data = input_path('DATA_RAMDISK', 'out/picasso/ram-data/data.dbrdisk')
    fd = input_path('FD', 'uefi/SILICIUM_UEFI.fd')
    addon = input_path('WF_ADDON', addon_name)
    trust = input_path('OWNER_TRUST')
    seed = os.environ.get('UNIQUE_CHIP_ID')
    if not seed:
        raise ValueError('Set UNIQUE_CHIP_ID to the same value used for the current kernel/payload')
    board_text = (ROOT / 'board.toml').read_text(encoding='utf-8')
    for variable, key in (('OWNED_MEMORY', 'owned_memory_size'), ('DRAM_SIZE', 'dram_size')):
        value = os.environ.get(variable)
        if value:
            if int(value, 0) <= 0:
                raise ValueError(f'{variable} must be positive')
            board_text, count = re.subn(rf'{key} = "[^"]*"', f'{key} = "{value}"', board_text, count=1)
            if count != 1:
                raise ValueError(f'Board field missing: {key}')
    board_text, count = re.subn(r'size_mib = 60\b', 'size_mib = 80', board_text, count=1)
    if count != 1:
        raise ValueError('Expected current 60 MiB payload layout')
    # The experiment uses physical touch; omit repeated synthetic swipe trials
    # to leave room in DarwinBoot's 1000-byte argument buffer, as in PR13.
    for argument in ('offline-hid-swipe-delay=8', 'offline-hid-auto-home-count=8', 'offline-hid-swipe=8'):
        board_text = board_text.replace(f'  "{argument}",\n', '')
    # Keep the ios16 policy and identity; only this temporary device view changes.
    tomllib.loads(board_text)
    args = ['sheng-cpufreq=4', 'sheng-gpu=4', 'sheng-gpu-metal=1', 'sheng-gpu-addon=1',
            'sheng-touch-hid=1', 'sheng-touch=4', 'sheng-touch-start=60']
    if os.environ.get('GRID', '1') != '0':
        args += ['sheng-display-grid=10']
    args += extra
    output = ROOT / 'out' / tag
    output.mkdir(parents=True, exist_ok=True)
    tree = runpy.run_path(str(ROOT / 'unixnu'))['prepare']()
    with tempfile.TemporaryDirectory(prefix='device-', dir=output) as temporary:
        device = Path(temporary)
        (device / 'board.toml').write_text(board_text, encoding='utf-8')
        for name in ('scripts', 'drivers', 'kits', 'package', 'patches', 'configs', 'policy', 'uefi', '.cache'):
            source = ROOT / name
            if source.exists():
                (device / name).symlink_to(source, target_is_directory=True)
        common = ['--device', str(device), '--local', str(ROOT), '--out', str(output), '--kernel', str(kernel)]
        command = [sys.executable, str(tree / 'unixnu'), 'build', 'payload', *common,
                   '--data-ramdisk', str(data), '--owner-trust-cache', str(trust), '--gpu-addon', str(addon),
                   '--unique-chip-id', seed]
        for arg in args:
            command += ['--kernel-arg', arg]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([sys.executable, str(tree / 'unixnu'), 'bootimg', *common,
                        '--fd', str(fd), '--dtb', str(ROOT / 'uefi/mu-silicium/Resources/DTBs/picasso-topology.dtb'),
                        '--ramdisk', str(output / 'picasso/payload/payload.img'),
                        '--output', str(output / f'picasso/boot/{tag}.img')], cwd=ROOT, check=True)
    print(output / f'picasso/boot/{tag}.img')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
