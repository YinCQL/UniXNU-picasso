#!/usr/bin/env python3
"""Execute replayed startup cache and brightness MAC policies; no SDK/device claims."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('extract', ROOT / 'scripts/check-picasso-buttons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = (args.xnu / 'bsd/kern/kern_exec.c').read_text()
    cache = module.extract(source, 'static void\npicasso_apps_cache_prepare(')
    (out / 'startup-cache.inc').write_text(cache, newline='\n')
    iokit = (args.xnu / 'iokit/Kernel/IOUserClient.cpp').read_text()
    mac = '\n'.join(module.extract(iokit, name) for name in
                    ('static bool\npicasso_brightness_caller(', 'static int\npicasso_iokit_set_properties_check('))
    (out / 'brightness-mac.inc').write_text(mac, newline='\n')
    display = (ROOT / 'package/PicassoDisplay.cpp').read_text()
    network = module.extract(display, 'static void picasso_network_stack_start(')
    (out / 'network-start.inc').write_text(network, newline='\n')
    late = module.extract(display, 'extern "C" void native_legacy_display_late_probe(')
    assert late.index('picasso_network_stack_start();') < late.index('if (!display) return;')
    # Also verify these tested functions are on the actual production dispatch paths.
    mount = module.extract(source, 'static void\nnative_picasso_apps_mount(')
    assert mount.count('picasso_apps_cache_prepare(p, scratch, found, resolved);') == 1
    entry = module.extract(iokit, 'is_io_registry_entry_set_properties\n(')
    assert entry.count('picasso_iokit_set_properties_check(entry, &obj)') == 1
    assert entry.index('picasso_iokit_set_properties_check') < entry.index('entry->setProperties( obj )')
    runs = []
    for fixture in ('picasso-startup-cache-host.c', 'picasso-brightness-mac-host.cpp', 'picasso-network-start-host.cpp'):
        cpp = fixture.endswith('.cpp')
        compiler = shutil.which('clang++' if cpp else 'clang')
        if not compiler and os.name == 'nt':
            compiler = str(Path('C:/Program Files/LLVM/bin') / ('clang++.exe' if cpp else 'clang.exe'))
        for sanitized in (False, True):
            name = Path(fixture).stem + ('-sanitized' if sanitized else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            cmd = [compiler, '-std=c++17' if cpp else '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-O1', '-I', str(out), str(ROOT / 'scripts/tests' / fixture), '-o', str(exe)]
            if sanitized:
                cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined']
            for stage, command in (('compile', cmd), ('run', [str(exe)])):
                result = subprocess.run(command, capture_output=True, text=True)
                (out / f'{name}-{stage}.log').write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                runs.append(dict(name=name, stage=stage, passed=True))
    inputs = [Path(__file__), ROOT / 'scripts/tests/picasso-startup-cache-host.c',
              ROOT / 'scripts/tests/picasso-brightness-mac-host.cpp',
              ROOT / 'scripts/tests/picasso-network-start-host.cpp', ROOT / 'package/PicassoDisplay.cpp',
              ROOT / 'patches/picasso-startup-cache-xnu.patch',
              ROOT / 'patches/picasso-persistent-records-xnu.patch',
              ROOT / 'patches/picasso-brightness-properties-xnu.patch']
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
        replayed={'kern_exec.c': hashlib.sha256(source.encode()).hexdigest(),
                  'IOUserClient.cpp': hashlib.sha256(iokit.encode()).hexdigest()}), indent=2) + '\n')
    print('PASS production startup cache and brightness property MAC policies')


if __name__ == '__main__':
    main()
