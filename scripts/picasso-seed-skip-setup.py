#!/usr/bin/env python3
"""Mark Setup as done in a copy of the Data seed for the selected iOS userland.

  picasso-seed-skip-setup.py BASE.dbr OUTPUT.dbr [--no-auto-dim]

Runs the upstream scripts/seed-data-skip-setup.py with this board's userland build and version
(picasso_target) instead of the iPadOS 17.7.11 / 21H461 pair it carries. com.apple.migration's
LastSystemVersion must name the system that boots: a mismatch makes the data migrator run on every
boot before SpringBoard can show anything. SpringBoard also gets BSUIVibrancyEffectViewDisabled
for the software renderer. Everything else is the upstream tool unchanged.
With --inject-lib PATH, that library is also copied into the Data seed at /picasso/ (runtime
/private/var/picasso/), where the kernel's picasso_inject_sb boot-arg points, so dyld loads it
into SpringBoard. The argument is consumed here; the upstream tool never sees it.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import picasso_target as target

INJECT_DIR = 'picasso'
INJECT_NAME = 'libpicasso-sb.dylib'


def main():
    inject_lib = None
    if '--inject-lib' in sys.argv:
        at = sys.argv.index('--inject-lib')
        if at + 1 >= len(sys.argv):
            raise ValueError('--inject-lib requires a library path')
        inject_lib = Path(sys.argv[at + 1])
        # The payload driver runs this copied script in the assembled tree;
        # board-relative outputs still belong to the device checkout.
        if not inject_lib.is_absolute():
            inject_lib = Path(os.environ.get('UNIXNU_DEVICE', os.getcwd())) / inject_lib
        inject_lib = inject_lib.resolve()
        if not inject_lib.is_file():
            raise FileNotFoundError(f'SpringBoard inject library not found: {inject_lib}')
        del sys.argv[at:at + 2]
    spec = importlib.util.spec_from_file_location('seed_data_skip_setup', HERE / 'seed-data-skip-setup.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    tool.BUILD, tool.VERSION = target.BUILD, target.VERSION
    # No GPU here: CoreAnimation's software renderer draws a "vibrant" view (the lock screen's
    # clock) as a red mask over an untoned wallpaper. BSUIVibrancyEffectViewDisabled makes such
    # views draw their content as it is, as HackiOS sets it for SpringBoard without a GPU.
    merge_pref = tool.merge_pref
    copied = {'done': False}

    def merge_springboard_pref(var, name, values):
        # The first merge_pref call gives the Data volume root (var); copy the inject library then.
        if inject_lib is not None and not copied['done']:
            copied['done'] = True
            if inject_lib.is_file():
                dest_dir = Path(var) / INJECT_DIR
                dest_dir.mkdir(parents=True, exist_ok=True)
                dest = dest_dir / INJECT_NAME
                shutil.copyfile(inject_lib, dest)
                os.chmod(dest, 0o644)
                print(f'picasso: inject library placed at /{INJECT_DIR}/{INJECT_NAME} ({dest.stat().st_size} bytes)')
            else:
                print(f'picasso: no inject library at {inject_lib}; SpringBoard runs unmodified')
        if name == 'com.apple.springboard':
            values = {**values, 'BSUIVibrancyEffectViewDisabled': True}
        merge_pref(var, name, values)

    tool.merge_pref = merge_springboard_pref
    print(f'picasso: Setup marked done for {target.PRODUCT} {target.VERSION} ({target.BUILD})')
    tool.main()


if __name__ == '__main__':
    main()
