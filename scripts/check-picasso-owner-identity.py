#!/usr/bin/env python3
"""Exercise the real config/header/caller path for owner boot-set identities.

Requires old and new replayed XNU trees. No SDK build, device I/O or crypto
qualification: the host publisher is a strict device/UUID stand-in. Original
publisher, verifier and the rest of XNU are separately compared byte for byte.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from types import SimpleNamespace

import picasso_target as target


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def function(source, name):
    tree = ast.parse(source)
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == name)
    return compile(ast.Module(body=[node], type_ignores=[]), name, 'exec')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--baseline-xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe')
    parser.add_argument('--bash', default=shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tree = args.tree.resolve()
    baseline = (tree / 'scripts/build-xnu-4k-baseline.sh').read_text()
    checker = load(root / 'scripts/check-picasso-target.py')
    checker.check_board(root)
    config = (root / 'configs/picasso_defconfig').read_text()
    key = 'CONFIG_XNU_OWNER_ROOT_VOLUME_UUID'
    setting = f'{key}="{target.OS_UUID}"'
    rejected = []
    for name, changed in [
            ('missing', config.replace(setting, '')),
            ('old-os', config.replace(target.OS_UUID, '04769D26-2CA0-4825-B443-E2E5F88300F2')),
            ('wrong-role', config.replace(target.OS_UUID, target.SYSTEMOS_UUID)),
            ('duplicate', config + '\n' + setting + '\n'),
            ('override', config + '\n' + key + '="04769D26-2CA0-4825-B443-E2E5F88300F2"\n')]:
        case = out / ('config-' + name)
        (case / 'configs').mkdir(parents=True)
        shutil.copy2(root / 'board.toml', case / 'board.toml')
        (case / 'configs/picasso_defconfig').write_text(changed)
        try:
            checker.check_board(case)
        except ValueError as error:
            assert 'owner boot-manifest OS UUID' in str(error)
            rejected.append(name)
        else:
            raise AssertionError('Invalid root identity accepted: ' + name)

    # Execute the actual defconfig parser and environment exporter functions.
    namespace = dict(re=re, Error=ValueError)
    exec(function((tree / 'scripts/unixnu/config.py').read_text(), 'parse_config'), namespace)
    parsed = namespace['parse_config'](config)
    exec(function((tree / 'scripts/unixnu/kernel.py').read_text(), 'xnu_env'), namespace)
    env = namespace['xnu_env'](SimpleNamespace(need_config=lambda: parsed, tree=tree))
    assert env['XNU_OWNER_ROOT_VOLUME_UUID'] == target.OS_UUID
    assert env['XNU_OWNER_CACHE_VOLUME_UUID'] == target.SYSTEMOS_UUID
    assert env['XNU_OWNER_BOOT_MANIFEST'] == '1'
    # Run the real shell invocation, Python header generator and object stamp.
    start = baseline.index('OWNER_DATA_HEADER=')
    end = baseline.index('OWNER_CRYPTEX_FILES=', start)
    block = baseline[start:end]
    assert '"$OWNER_BOOT_MANIFEST" "${XNU_OWNER_ROOT_VOLUME_UUID:-}"' in block
    assert '-include $OWNER_DATA_HEADER' in baseline
    shell_file = out / 'header-and-stamp.sh'
    python_path = sys.executable.replace('\\', '/')
    shell_file.write_text('set -eu\npython3() { "' + python_path + '" "$@"; }\n' + block,
                          encoding='utf-8', newline='\n')
    fixture = out / 'build-fixture'
    object_path = fixture / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o'
    object_path.parent.mkdir(parents=True)
    object_path.write_bytes(b'old object sentinel')
    run_env = {**os.environ, **env, 'ROOT': fixture.as_posix(), 'OWNER_DATA_MOUNT': '0',
               'NATIVE_RAM_BLOCK': '1', 'OWNER_BOOT_MANIFEST': '1'}
    # Keep the optional Data path enabled in production; the standalone header
    # fixture disables only Data generation to avoid needing a formatted seed.
    def generate(name, **overrides):
        result = subprocess.run([args.bash, str(shell_file)], env={**run_env, **overrides},
                                capture_output=True, timeout=20)
        (out / (name + '.stdout.txt')).write_bytes(result.stdout)
        (out / (name + '.stderr.txt')).write_bytes(result.stderr)
        return result
    assert generate('header-valid').returncode == 0
    header = fixture / 'build/xnu/owner-data-volume.h'
    assert f'#define XNU_OWNER_ROOT_VOLUME_UUID "{target.OS_UUID}"' in header.read_text()
    assert not object_path.exists()
    retained = list((fixture / 'build/xnu/stale-owner-data').glob('*/kern_exec.o'))
    assert len(retained) == 1 and retained[0].read_bytes() == b'old object sentinel'
    object_path.write_bytes(b'new object sentinel')
    stamp = header.stat().st_mtime_ns
    assert generate('header-unchanged').returncode == 0
    assert object_path.read_bytes() == b'new object sentinel' and header.stat().st_mtime_ns == stamp
    for name, value in [('empty', ''), ('malformed', 'not-a-uuid'),
                        ('zero', '00000000-0000-0000-0000-000000000000'),
                        ('same-volume', target.SYSTEMOS_UUID)]:
        before = header.read_bytes()
        result = generate('header-' + name, XNU_OWNER_ROOT_VOLUME_UUID=value)
        assert result.returncode != 0 and b'XNU_OWNER_ROOT_VOLUME_UUID' in result.stderr
        assert header.read_bytes() == before and object_path.read_bytes() == b'new object sentinel'
    # A valid identity change must invalidate, retaining rather than deleting.
    assert generate('header-changed', XNU_OWNER_ROOT_VOLUME_UUID='04769D26-2CA0-4825-B443-E2E5F88300F2').returncode == 0
    assert not object_path.exists()
    retained = list((fixture / 'build/xnu/stale-owner-data').glob('*/kern_exec.o'))
    assert sorted(p.read_bytes() for p in retained) == [b'new object sentinel', b'old object sentinel']
    assert generate('header-restored').returncode == 0
    assert generate('header-disabled', OWNER_BOOT_MANIFEST='0', XNU_OWNER_ROOT_VOLUME_UUID='').returncode == 0
    assert 'XNU_OWNER_ROOT_VOLUME_UUID' not in header.read_text()
    data_uuid = '11111111-2222-3333-4444-555555555555'
    assert generate('header-with-data', OWNER_DATA_MOUNT='1', XNU_OWNER_DATA_VOLUME_UUID=data_uuid).returncode == 0
    assert f'#define XNU_OWNER_DATA_VOLUME_UUID "{data_uuid}"' in header.read_text()
    assert f'#define XNU_OWNER_ROOT_VOLUME_UUID "{target.OS_UUID}"' in header.read_text()
    assert generate('header-final').returncode == 0

    old = (args.baseline_xnu / 'bsd/kern/kern_exec.c').read_text()
    new = (args.xnu / 'bsd/kern/kern_exec.c').read_text()
    pattern = r'\tif \(!error && cache_authorized && root_recheck &&\n\t    !native_owner_publish_boot_manifest\(.*?\n\t        selected, volume\)\) error = EAUTH;'
    old_call = re.findall(pattern, old, re.S)
    new_call = re.findall(pattern, new, re.S)
    assert len(old_call) == len(new_call) == 1
    guard = '#ifndef XNU_OWNER_ROOT_VOLUME_UUID\n#error Supply the signed OS volume UUID for owner boot metadata\n#endif'
    assert guard in new and guard not in old
    assert new_call[0] == old_call[0].replace('"04769D26-2CA0-4825-B443-E2E5F88300F2"', 'XNU_OWNER_ROOT_VOLUME_UUID')
    fixture_source = '''#include <string.h>
#include <stdio.h>
#define EAUTH 80
static int calls, force_failure;
static int native_owner_publish_boot_manifest(unsigned r, const char *ru, unsigned c, const char *cu) {
    ++calls;
    return !force_failure && r == 0x1000009 && c == 0x1000005 &&
        strcmp(ru, "ROOT_UUID") == 0 && strcmp(cu, "CACHE_UUID") == 0;
}
'''.replace('ROOT_UUID', target.OS_UUID).replace('CACHE_UUID', target.SYSTEMOS_UUID)
    for name, call in [('old_binding', old_call[0]), ('new_binding', new_call[0])]:
        fixture_source += ('static int ' + name + '(int error, int cache_authorized, int root_recheck, unsigned root_device, unsigned selected, const char *volume) {\n'
                           'if (!root_recheck) error = EAUTH;\n' + call + '\nreturn error;\n}\n')
    fixture_source += '''#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\\n", __LINE__); return 1; } } while (0)
int main(void) {
    CHECK(old_binding(0,1,1,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    CHECK(new_binding(0,1,1,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==0);
    CHECK(new_binding(0,1,1,0x1000005,0x1000009,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    CHECK(new_binding(0,1,1,0x1000009,0x1000009,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    CHECK(new_binding(0,1,1,0x1000009,0x1000005,XNU_OWNER_ROOT_VOLUME_UUID)==80);
    force_failure=1;
    CHECK(new_binding(0,1,1,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    int previous=calls;
    CHECK(new_binding(5,1,1,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==5);
    CHECK(new_binding(80,0,1,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    CHECK(new_binding(0,1,0,0x1000009,0x1000005,XNU_OWNER_CACHE_VOLUME_UUID)==80);
    CHECK(calls==previous);
    puts("PASS: old UUID reproduces EAUTH; configured identity passes; failures stay closed");
    return 0;
}
'''
    source = out / 'actual-owner-call.c'
    source.write_text(guard + '\n' + fixture_source, encoding='utf-8')
    exe = out / ('actual-owner-call.exe' if os.name == 'nt' else 'actual-owner-call')
    command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-include', str(header), str(source), '-o', str(exe)]
    compile_result = subprocess.run(command, capture_output=True)
    (out / 'compile.txt').write_bytes(compile_result.stdout + compile_result.stderr)
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(exe)], capture_output=True)
    (out / 'run.txt').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout
    missing = subprocess.run([args.cc, '-fsyntax-only', str(source)], capture_output=True)
    (out / 'missing-header.txt').write_bytes(missing.stdout + missing.stderr)
    assert missing.returncode != 0 and b'Supply the signed OS volume UUID' in missing.stderr
    report = dict(passed=True, scope=__doc__, rejected_configs=rejected, actual_config_environment=True,
                  actual_header_and_stamp_executed=True, invalid_header_preserves_existing_files=True,
                  same_identity_keeps_object=True, changed_identity_retains_stale_object=True,
                  compiled_actual_old_and_new_calls=True, missing_identity_compile_rejected=True,
                  publisher_is_stand_in=True, compiler_command=command,
                  expected_boot_set_sha384=hashlib.sha384(b'VerifiedRoot.OwnerBootSet.v1\0' + b'\1' +
                      hashlib.sha384((root / 'policy/verified-root-picasso/os/manifest.bin').read_bytes()).digest() + b'\2' +
                      hashlib.sha384((root / 'policy/verified-root-picasso/systemos/manifest.bin').read_bytes()).digest()).hexdigest())
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: config -> environment -> header -> compiled owner call; stale objects and rejection paths')


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
