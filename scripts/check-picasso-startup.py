#!/usr/bin/env python3
"""Execute replayed startup cache and brightness MAC policies; no SDK/device claims."""
import argparse
import hashlib
import importlib.util
import json
import os
import ast
import plistlib
import tomllib
import re
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
                    ('static int\npicasso_iokit_set_properties_check(',))
    (out / 'brightness-mac.inc').write_text(mac, newline='\n')
    display = (ROOT / 'package/PicassoDisplay.cpp').read_text()
    network = module.extract(display, 'static void picasso_network_stack_start(')
    (out / 'network-start.inc').write_text(network, newline='\n')
    plist_start = source.index('static const char picasso_ifnamer_plist[] =')
    plist_end = source.index('\n\n', plist_start)
    literal = source[plist_start:plist_end]
    xml = ''.join(ast.literal_eval(line.strip().rstrip(';')) for line in literal.splitlines()[1:])
    config = plistlib.loads(xml.encode())
    assert config['WaitStackTimeout'] == config['WaitQuietTimeout'] == 5
    assert config['Builtin'] is True and config['CFBundleIdentifier'] == 'com.apple.SystemConfiguration.InterfaceNamer'
    assert config['CFBundleExecutable'] == 'InterfaceNamer' and config['CFBundleVersion'] == '1.21'
    reader = module.extract(source, 'static int\npicasso_ifnamer_read(')
    assert 'VATTR_WANTED(&va, va_data_size)' in reader and 'VATTR_IS_SUPPORTED(&va, va_data_size)' in reader
    assert 'O_NOFOLLOW' in reader and 'vnode_vtype(vp) != VREG' in reader
    (out / 'network-reader.inc').write_text(reader, newline='\n')
    policy = literal + '\n' + '\n'.join(module.extract(source, name) for name in
        ('static int\npicasso_ifnamer_directory(', 'static int\npicasso_ifnamer_metadata(', 'static void\npicasso_ifnamer_mount('))
    (out / 'network-config.inc').write_text(policy, newline='\n')
    data_prepare = module.extract(source, 'static int\nnative_owner_data_prepare(')
    assert data_prepare.count('picasso_ifnamer_mount(p, scratch);') == 1
    observe = module.extract(iokit, 'static void\npicasso_network_busy_observe(')
    (out / 'network-observe.inc').write_text(observe, newline='\n')
    busy = module.extract(iokit, 'is_io_service_get_busy_state(')
    assert busy.count('picasso_network_busy_observe(service, *busyState);') == 1

    late = module.extract(display, 'extern "C" void native_legacy_display_late_probe(')
    assert late.index('picasso_network_stack_start();') < late.index('if (!display) return;')
    # Also verify these tested functions are on the actual production dispatch paths.
    mount = module.extract(source, 'static void\nnative_picasso_apps_mount(')
    assert mount.count('picasso_apps_cache_prepare(p, scratch, found, resolved);') == 1
    entry = module.extract(iokit, 'is_io_registry_entry_set_properties\n(')
    assert entry.count('picasso_iokit_set_properties_check(entry, &obj)') == 1
    assert entry.index('picasso_iokit_set_properties_check') < entry.index('entry->setProperties( obj )')
    # Keep seed identity unchanged while applying only the approved runtime job release.
    board = tomllib.loads((ROOT / 'board.toml').read_text())['payload']
    assert board['runtime_enabled_jobs'] == ['com.apple.bluetoothd']
    before = plistlib.dumps({job: True for job in board['offline_disabled_jobs']})
    after = plistlib.dumps({job: True for job in board['offline_disabled_jobs'] if job not in board['runtime_enabled_jobs']})
    constants = source[source.index('static const uint8_t picasso_jobs_before_sha'):source.index('static int\npicasso_jobs_match(')]
    for name, data in [('before', before), ('after', after)]:
        literal = re.search('picasso_jobs_' + name + r'_sha\[32\] = \{(.*?)\}', constants)[1]
        assert bytes(int(n, 16) for n in literal.split(',')) == hashlib.sha256(data).digest()
    policy = constants + '\n'.join(module.extract(source, name) for name in
        ('static int\npicasso_jobs_match(', 'static int\npicasso_jobs_metadata(', 'static int\npicasso_jobs_sync(', 'static int\npicasso_jobs_enable('))
    (out / 'runtime-jobs.inc').write_text(policy, newline='\n')
    (out / 'jobs-data.inc').write_text('\n'.join('static const unsigned char jobs_' + name + '[] = {' + ','.join(str(n) for n in data) + '};' for name, data in [('before', before), ('after', after)]), newline='\n')
    assert data_prepare.count('picasso_jobs_enable(p, scratch)') == 1
    assert data_prepare.index('native_staged_apps_prepare(') < data_prepare.index('picasso_jobs_enable(')
    runs = []
    for fixture in ('picasso-startup-cache-host.c', 'picasso-brightness-mac-host.cpp', 'picasso-network-start-host.cpp', 'picasso-network-config-host.c', 'picasso-network-observe-host.cpp', 'picasso-network-reader-host.c', 'picasso-runtime-jobs-host.c'):
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
    inputs = [Path(__file__), ROOT / 'board.toml', ROOT / 'unixnu', ROOT / 'patches/picasso-runtime-jobs-xnu.patch', ROOT / 'scripts/tests/picasso-runtime-jobs-host.c', ROOT / 'scripts/tests/picasso-startup-cache-host.c',
              ROOT / 'scripts/tests/picasso-brightness-mac-host.cpp',
              ROOT / 'scripts/tests/picasso-network-start-host.cpp', ROOT / 'scripts/tests/picasso-network-config-host.c',
              ROOT / 'scripts/tests/picasso-network-observe-host.cpp', ROOT / 'scripts/tests/picasso-network-reader-host.c', ROOT / 'package/PicassoDisplay.cpp',
              ROOT / 'patches/picasso-startup-cache-xnu.patch',
              ROOT / 'patches/picasso-persistent-records-xnu.patch',
              ROOT / 'patches/picasso-brightness-properties-xnu.patch', ROOT / 'patches/picasso-network-timeout-xnu.patch']
    (out / 'results.json').write_text(json.dumps(dict(passed=True, scope=__doc__, runs=runs,
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
        replayed={'kern_exec.c': hashlib.sha256(source.encode()).hexdigest(),
                  'IOUserClient.cpp': hashlib.sha256(iokit.encode()).hexdigest()}), indent=2) + '\n')
    print('PASS production startup cache and brightness property MAC policies')


if __name__ == '__main__':
    main()
