#!/usr/bin/env python3
"""Replay the device's pinned patch chain twice in an isolated directory.

No SDK, device writes, network fetches or builds. --upstream and --xnu are Git
checkouts with the pinned commits available. --prelink-patches is the patches/
directory of kits/NativePrelink at the commit in upstream's tree. --output must
not exist; all generated files are kept there for inspection.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

DEVICE = Path(__file__).resolve().parents[1]
REPLAY_SOURCES = (
    'bsd/kern/kern_exec.c', 'bsd/vm/vm_unix.c', 'bsd/dev/picasso_usb.c',
    'bsd/kern/kern_exit.c', 'bsd/vfs/vfs_syscalls.c', 'bsd/vfs/vfs_lookup.c',
    'bsd/kern/sys_generic.c', 'bsd/kern/kern_descrip.c',
    'bsd/kern/kern_mman.c', 'bsd/kern/ubc_subr.c',
    'osfmk/arm64/sleh.c', 'iokit/Kernel/IOUserClient.cpp',
    'bsd/vfs/vfs_attrlist.c', 'bsd/kern/bsd_init.c', 'osfmk/arm/rtclock.c',
    'osfmk/kern/task.c',
)


def git(repo, *args, check=True):
    return subprocess.run(['git', '-C', str(repo), *map(str, args)], check=check,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def export(repo, revision, out):
    archive = out.with_suffix('.zip')
    git(repo, '-c', 'core.autocrlf=false', 'archive', '--format=zip', '--output=' + str(archive), revision)
    with zipfile.ZipFile(archive) as z:
        z.extractall(out)
    git(out, 'init', '-q')
    git(out, 'config', 'core.autocrlf', 'false')


def apply(tree, patch):
    result = git(tree, 'apply', '--check', patch, check=False)
    if result.returncode:
        raise RuntimeError(f'{patch.name}: {result.stderr.decode("utf-8", "replace")}')
    git(tree, 'apply', '--whitespace=nowarn', patch)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--upstream', type=Path, required=True)
    p.add_argument('--xnu', type=Path, required=True)
    p.add_argument('--prelink-patches', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    revision = (DEVICE / 'upstream.rev').read_text().strip()
    tree, xnu = out / 'tree', out / 'xnu'
    export(a.upstream, revision, tree)
    # Normalize the Windows checkout just as the committed Linux/macOS blob.
    outer = out / 'device.patch'
    outer.write_text((DEVICE / 'patches/unixnu-source-kernel.patch').read_text(encoding='utf-8'),
                     encoding='utf-8', newline='\n')
    apply(tree, outer)
    usb = (DEVICE / 'patches/picasso-usb-console-xnu.patch').read_text(encoding='utf-8')
    (tree / 'patches/xnu/zzz-picasso-usb-console.patch').write_text(usb, encoding='utf-8', newline='\n')
    baseline = (tree / 'scripts/build-xnu-4k-baseline.sh').read_text()
    xnu_rev = re.search(r'XNU_COMMIT=\$\{UNIXNU_XNU_COMMIT:-([0-9a-f]{40})\}', baseline)[1]
    export(a.xnu, xnu_rev, xnu)
    late_block = baseline.split('for LATE_PATCH in ', 1)[1].split('; do', 1)[0]
    late = [tree / path for path in re.findall(r'"\$ROOT/([^"\n]+)"', late_block)]
    # These diagnostics modify functions already touched by the owner and
    # boot-progress patches. Reverse them first on incremental builds.
    diagnostics = tree / 'patches/xnu/zzzzz-picasso-launchd-diagnostics.patch'
    assert diagnostics.is_file(), 'Missing launchd diagnostics patch'
    assert late[0] == diagnostics, 'Launchd diagnostics must be reversed first'
    assert late[1] == tree / 'patches/xnu/zzzz-picasso-systemos-union.patch'
    prelink = sorted(a.prelink_patches.resolve().glob('*.patch'))
    if not prelink:
        raise RuntimeError('No prelink patches; refusing an incomplete replay')
    patches = sorted((tree / 'patches/xnu').glob('*.patch')) + prelink
    passes = []
    previous_sources = None
    for pass_number in (1, 2):
        for patch in late:
            if git(xnu, 'apply', '--reverse', '--check', patch, check=False).returncode == 0:
                git(xnu, 'apply', '--reverse', '--whitespace=nowarn', patch)
        applied = 0
        for patch in patches:
            if git(xnu, 'apply', '--reverse', '--check', patch, check=False).returncode:
                apply(xnu, patch)
                applied += 1
        sources = tuple((xnu / p).read_bytes() for p in REPLAY_SOURCES)
        if previous_sources is not None:
            assert sources == previous_sources, 'Second pass changed generated source bytes'
        previous_sources = sources
        passes.append({'pass': pass_number, 'patches': len(patches), 'applied': applied})
        print(f'PASS {pass_number}: {len(patches)} patches checked, {applied} applied')
    kern = (xnu / 'bsd/kern/kern_exec.c').read_text()
    vm = (xnu / 'bsd/vm/vm_unix.c').read_text()
    assert kern.count('native_owner_cache_prepare(proc_t p,') == 1
    for stage in range(1, 7):
        assert kern.count(f'NATIVE_OWNER_CACHE stage={stage} ') == 1
    assert kern.count('NATIVE_OWNER_CACHE done stage=') == 1
    assert vm.count('native_owner_shared_cache_carrier(vnode_t process_root,') == 1
    assert 'NATIVE_OWNER_CARRIER root=%x cache=%x accepted=%u' in vm
    config = (DEVICE / 'configs/picasso_defconfig').read_text()
    for selector in ('OWNER_ROOT_TRUST', 'OWNER_CACHE_MOUNT', 'OWNER_CACHE_STARTUP',
                     'OWNER_BOOT_MANIFEST', 'OWNER_UNION_SYSTEMOS'):
        assert config.count('CONFIG_XNU_' + selector + '=y') == 1
        assert '# CONFIG_XNU_' + selector + ' is not set' not in config
    assert config.count('# CONFIG_XNU_OWNER_CACHE_CARRIER is not set') == 1
    assert 'CONFIG_XNU_OWNER_CACHE_CARRIER=y' not in config
    assert '-DXNU_OWNER_UNION_SYSTEMOS=$OWNER_UNION_SYSTEMOS' in baseline
    assert 'request.flags |= MNT_UNION;' in kern
    assert 'cache_authorized = cache_authorized && native_union;' in kern
    assert 'native_owner_union_path("/usr/lib/libobjc-trampolines.dylib", cache_mount)' in kern
    assert 'native_owner_union_path("/sbin/launchd", root_mount)' in kern
    assert 'p == kernproc && rootvnode == olddp' in (xnu / 'bsd/vfs/vfs_syscalls.c').read_text()
    assert '-DXNU_OWNER_CACHE_CARRIER=$OWNER_CACHE_CARRIER' in baseline
    assert 'UFS_ARCHIVE=$ROOT/build/native-ufs/libNativeUfs.a' in baseline
    assert '#define PREBOOT_CRYPTEX_PATH "/private/preboot/Cryptexes"' in vm
    assert '&& !owner_carrier' in vm
    # The wrapper must still emit the canonical USB source alongside the
    # owner patches. Host execution of watchdog logic is a separate check.
    assert (xnu / 'bsd/dev/picasso_usb.c').is_file()
    hashes = {p: hashlib.sha256((xnu / p).read_bytes()).hexdigest()
              for p in REPLAY_SOURCES}
    report = {'upstream': revision, 'xnu': xnu_rev, 'passes': passes, 'source_hashes': hashes,
              'device_patch_sha256': hashlib.sha256(outer.read_bytes()).hexdigest(),
              'usb_patch_sha256': hashlib.sha256(usb.encode('utf-8')).hexdigest(),
              'prelink_patches': {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in prelink},
              'scope': 'patch application and integration only; not compilation or device validation'}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: single mount/carrier implementation, stage logs and pinned build wiring')


if __name__ == '__main__':
    main()
