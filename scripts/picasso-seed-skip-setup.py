#!/usr/bin/env python3
"""Mark Setup as done in a copy of the Data seed and add the Picasso UI prefs.

  picasso-seed-skip-setup.py BASE.dbr OUTPUT.dbr [--no-auto-dim] [--statusbar-provider CLASS] [--inject-lib PATH]

Adapted from the upstream feat/ios16 wrapper to this board's 21H461 userland.
Runs the UniXNU scripts/seed-data-skip-setup.py (copied into the assembled build
tree) with this board's build number so com.apple.migration's
LastSystemVersion names the system that boots: a mismatch makes the data
migrator run on every boot before SpringBoard can show anything.

SpringBoard also gets BSUIVibrancyEffectViewDisabled for the software renderer.
With --statusbar-provider, com.apple.UIKit selects the reviewed rectangular
Pad_ForcedNoCell layout instead of UIKit's Fallback (the red bar). With
--inject-lib PATH, that library is copied into the Data seed at
/picasso/libpicasso-sb.dylib (runtime /private/var/picasso/libpicasso-sb.dylib),
the path the kernel's picasso_inject_sb=1 default loads into SpringBoard.
--inject-lib is consumed here; the upstream tool never sees it.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The userland this Data seed belongs to (board.toml [ipsw]).
BUILD = '21H461'
VERSION = '17.7.11'
INJECT_DIR = 'picasso'
INJECT_NAME = 'libpicasso-sb.dylib'
STATUSBAR_PROVIDER = '_UIStatusBarVisualProvider_Pad_ForcedNoCell'


def consume_statusbar_provider(arguments):
    if '--statusbar-provider' not in arguments:
        return None
    if arguments.count('--statusbar-provider') != 1:
        raise ValueError('Duplicate --statusbar-provider')
    at = arguments.index('--statusbar-provider')
    if at + 1 >= len(arguments) or arguments[at + 1] != STATUSBAR_PROVIDER:
        raise ValueError('--statusbar-provider requires the reviewed rectangular provider')
    value = arguments[at + 1]
    del arguments[at:at + 2]
    return value


def consume_inject_lib(arguments):
    if '--inject-lib' not in arguments:
        return None
    if arguments.count('--inject-lib') != 1:
        raise ValueError('Duplicate --inject-lib')
    at = arguments.index('--inject-lib')
    if at + 1 >= len(arguments):
        raise ValueError('--inject-lib requires a library path')
    path = Path(arguments[at + 1])
    # The payload driver runs this copied script from the assembled tree;
    # board-relative outputs still belong to the device checkout.
    if not path.is_absolute():
        path = Path(os.environ.get('UNIXNU_DEVICE', os.getcwd())) / path
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(f'SpringBoard inject library not found: {path}')
    del arguments[at:at + 2]
    return path


def main():
    statusbar_provider = consume_statusbar_provider(sys.argv)
    inject_lib = consume_inject_lib(sys.argv)
    spec = importlib.util.spec_from_file_location('seed_data_skip_setup', HERE / 'seed-data-skip-setup.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    tool.BUILD, tool.VERSION = BUILD, VERSION
    configure_seed(tool, inject_lib, statusbar_provider)
    print(f'picasso: Setup marked done for {BUILD} ({VERSION})')
    tool.main()


def configure_seed(tool, inject_lib=None, statusbar_provider=None):
    # Render vibrant content directly with the software renderer.
    merge_pref = tool.merge_pref
    initialized = False

    def merge_springboard_pref(var, name, values):
        nonlocal initialized
        # __UIKitUserDefaults uses initWithSuiteName:@"com.apple.UIKit".
        # Its provider override bypasses the screen-size lookup that selects the fallback label.
        if not initialized:
            initialized = True
            if statusbar_provider is not None:
                if statusbar_provider != STATUSBAR_PROVIDER:
                    raise ValueError('Unreviewed status bar provider')
                merge_pref(var, 'com.apple.UIKit', {'UIStatusBarVisualProviderClassName': statusbar_provider,
                                                  'StatusBarHidesDate': True})
            if inject_lib is not None:
                dest_dir = Path(var) / INJECT_DIR
                dest_dir.mkdir(parents=True, exist_ok=True)
                os.chmod(dest_dir, 0o755)
                dest = dest_dir / INJECT_NAME
                shutil.copyfile(inject_lib, dest)
                os.chmod(dest, 0o644)
                print(f'picasso: inject library placed at /{INJECT_DIR}/{INJECT_NAME} ({dest.stat().st_size} bytes)')
        if name == 'com.apple.springboard':
            values = {**values, 'BSUIVibrancyEffectViewDisabled': True}
            # Some UIKit builds look the provider up through the process's own
            # defaults instead of the framework suite; keep both copies in sync.
            if statusbar_provider is not None:
                values = {**values, 'UIStatusBarVisualProviderClassName': statusbar_provider,
                          'StatusBarHidesDate': True}
        merge_pref(var, name, values)

    tool.merge_pref = merge_springboard_pref


if __name__ == '__main__':
    main()
