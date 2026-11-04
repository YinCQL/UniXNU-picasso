#!/usr/bin/env python3
"""Mark Setup as done in a copy of the Data seed for the selected iOS userland.

  picasso-seed-skip-setup.py BASE.dbr OUTPUT.dbr [--no-auto-dim] [--statusbar-provider CLASS]

Runs the upstream scripts/seed-data-skip-setup.py with this board's userland build and version
(picasso_target) instead of the iPadOS 17.7.11 / 21H461 pair it carries. com.apple.migration's
LastSystemVersion must name the system that boots: a mismatch makes the data migrator run on every
boot before SpringBoard can show anything. SpringBoard also gets BSUIVibrancyEffectViewDisabled
for the software renderer. Everything else is the upstream tool unchanged.
With --inject-lib PATH, that library is also copied into the Data seed at /picasso/ (runtime
/private/var/picasso/), where the kernel's picasso_inject_sb boot-arg points, so dyld loads it
into SpringBoard and, with picasso_inject_apps=1, main app executables. The argument
is consumed here; the upstream tool never sees it.
--statusbar-provider selects the reviewed native UIKit status bar layout for this GOP mode.
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


def main():
    if sys.platform.startswith('linux'):
        from picasso_linux_inputs import stage_setup
        stage_setup(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:], HERE.parent)
        return
    statusbar_provider = consume_statusbar_provider(sys.argv)
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
    configure_seed(tool, inject_lib, statusbar_provider)
    print(f'picasso: Setup marked done for {target.PRODUCT} {target.VERSION} ({target.BUILD})')
    tool.main()


def configure_seed(tool, inject_lib=None, statusbar_provider=None):
    # Render vibrant content directly with the software renderer.
    merge_pref = tool.merge_pref
    initialized = False

    def merge_springboard_pref(var, name, values):
        nonlocal initialized
        # __UIKitUserDefaults uses initWithSuiteName:@"com.apple.UIKit" in 20H392.
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
                dest = dest_dir / INJECT_NAME
                shutil.copyfile(inject_lib, dest)
                os.chmod(dest, 0o644)
                print(f'picasso: inject library placed at /{INJECT_DIR}/{INJECT_NAME} ({dest.stat().st_size} bytes)')
        if name == 'com.apple.springboard':
            values = {**values, 'BSUIVibrancyEffectViewDisabled': True}
        merge_pref(var, name, values)

    tool.merge_pref = merge_springboard_pref


if __name__ == '__main__':
    main()
