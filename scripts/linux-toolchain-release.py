#!/usr/bin/env python3
"""Package and install a pinned Ubuntu 24.04 x86_64 UniXNU toolchain Release."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

SCHEMA = 'picasso-linux-toolchain-v1'
ARCHIVE = 'picasso-linux-toolchain-ubuntu24-x86_64.tar.gz'
MANIFEST = 'linux-toolchain-manifest.json'
PREFIX = Path('/home/runner/work/_temp/unixnu-linux')
REQUIRED = ('llvm/bin/clang', 'llvm/bin/llvm-objcopy', 'CommandLineTools/usr/bin/ld',
            'CommandLineTools/usr/bin/libtool', 'CommandLineTools/usr/bin/mig',
            'CommandLineTools/usr/libexec/migcom', 'XcodeDefault.xctoolchain/usr/bin/iig',
            'CommandLineTools/usr/lib/libmacho.a', 'CommandLineTools/usr/lib/libstuff.a',
            'CommandLineTools/usr/libdispatch/lib/libdispatch.so', 'bin/lzfse')


def run(args, **kw):
    return subprocess.check_output([str(x) for x in args], timeout=180, **kw)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def setup_module(tree):
    spec = importlib.util.spec_from_file_location('linux_setup', tree / 'scripts/linux/setup-toolchain.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def identity(tree, prefix=PREFIX):
    host = run(['llvm-config', '--version', '--bindir', '--libdir'], text=True).strip()
    sha = hashlib.sha256()
    for value in (str(prefix), host):
        sha.update(value.encode() + b'\0')
    root = tree / 'scripts/linux'
    for path in sorted(root.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            sha.update(path.relative_to(root).as_posix().encode() + b'\0')
            sha.update(path.read_bytes() + b'\0')
    return {'recipe_sha256': sha.hexdigest(), 'host_llvm': host, 'prefix': str(prefix)}


def require_platform():
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise ValueError('This Release requires an Ubuntu 24.04 x86_64 runner')
    release = platform.freedesktop_os_release()
    if release.get('ID') != 'ubuntu' or release.get('VERSION_ID') != '24.04':
        raise ValueError('This Release requires Ubuntu 24.04')
    if platform.libc_ver()[0] != 'glibc':
        raise ValueError('This Release requires glibc')


def inventory(prefix):
    files, links = {}, {}
    for path in sorted(prefix.rglob('*')):
        name = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            links[name] = os.readlink(path)
        elif path.is_file():
            files[name] = digest(path)
    return files, links


def qualify(tree, prefix):
    """Install only wrappers; never invoke any compiler/toolchain source build."""
    require_platform()
    if prefix.resolve() != PREFIX or os.geteuid():
        raise ValueError(f'Install as root at the fixed runner prefix {PREFIX}')
    for name in REQUIRED:
        path = prefix / name
        if not path.is_file():
            raise ValueError(f'Incomplete toolchain: {name}')
        if '/bin/' in name or '/libexec/' in name:
            if not os.access(path, os.X_OK):
                raise ValueError(f'Toolchain program is not executable: {name}')
    setup = setup_module(tree)
    host = setup.host_tools()
    clt = prefix / 'CommandLineTools/usr'
    xct = prefix / 'XcodeDefault.xctoolchain/usr'
    setup.shims(clt, xct, prefix / 'llvm/bin/clang')
    setup.host_cc(clt, clt.parent / 'SDKs/MacOSX.sdk/usr/include', host)
    with tempfile.TemporaryDirectory(prefix='toolchain-verify-') as tmp:
        scratch = Path(tmp)
        cc = prefix / 'llvm/bin/clang'
        probe = '#if !__has_builtin(__builtin_xnu_type_summary)\n#error missing XNU builtins\n#endif\n'
        run([cc, '-target', 'arm64-apple-ios17.0', '-E', '-P', '-x', 'c', '-'], input=probe, text=True)
        obj = scratch / 'probe.o'
        run([cc, '-target', 'arm64-apple-ios17.0', '-O0', '-c', '-x', 'c', '-', '-o', obj],
            input='static int local_probe(void) { return 7; } int probe(void) { return local_probe(); }\n', text=True)
        edited = scratch / 'edited.o'
        run([prefix / 'llvm/bin/llvm-objcopy', '--globalize-symbol=_local_probe', obj, edited])
        if '_local_probe' not in run(['llvm-nm', '-g', edited], text=True):
            raise ValueError('Mach-O symbol globalization failed')
        linked = scratch / 'linked.o'
        run([clt / 'bin/ld', '-r', '-arch', 'arm64', '-ios_version_min', '17.0', '-o', linked, edited])
        if linked.read_bytes()[:4] != b'\xcf\xfa\xed\xfe':
            raise ValueError('ld64 did not produce a Mach-O object')
        executable = scratch / 'host-probe'
        run([clt / 'bin/xnu-host-cc', '-x', 'c', '-', '-o', executable], input='int main(void) { return 0; }\n', text=True)
        run([executable])
        for name in ('CommandLineTools/usr/bin/ld', 'XcodeDefault.xctoolchain/usr/bin/iig'):
            result = subprocess.run(['ldd', str(prefix / name)], capture_output=True, text=True, check=True)
            if 'not found' in result.stdout:
                raise ValueError(f'Runtime library missing for {name}: {result.stdout}')
        plain = scratch / 'plain'
        plain.write_bytes(b'Picasso toolchain LZFSE roundtrip\n' * 64)
        run([prefix / 'bin/lzfse', '-encode', '-i', plain, '-o', scratch / 'compressed'])
        run([prefix / 'bin/lzfse', '-decode', '-i', scratch / 'compressed', '-o', scratch / 'decoded'])
        if plain.read_bytes() != (scratch / 'decoded').read_bytes():
            raise ValueError('LZFSE roundtrip failed')
    print('PASS: XNU builtins, ARM64 compile/link, Mach-O objcopy, host wrapper and LZFSE', flush=True)


def add_notices(tree, prefix):
    setup = setup_module(tree)
    directory = prefix / 'share/unixnu-linux'
    directory.mkdir(parents=True, exist_ok=True)
    sources = [('llvm/llvm-project', setup.LLVM_COMMIT, 'llvm/LICENSE.TXT', 'LLVM-LICENSE.txt'),
               ('tpoechtrager/cctools-port', setup.CCTOOLS_PORT_REF, 'cctools/APPLE_LICENSE', 'cctools-APPLE_LICENSE'),
               ('tpoechtrager/cctools-port', setup.CCTOOLS_PORT_REF, 'cctools/COPYING', 'cctools-COPYING'),
               ('tpoechtrager/cctools-port', setup.CCTOOLS_PORT_REF, 'cctools/ld64/APPLE_LICENSE', 'ld64-APPLE_LICENSE'),
               ('apple-oss-distributions/bootstrap_cmds', setup.BOOTSTRAP_CMDS_TAG, 'APPLE_LICENSE', 'MIG-APPLE_LICENSE'),
               ('Nyktis/iig-port', setup.IIG_PORT_COMMIT, 'LICENSE', 'IIG-LICENSE'),
               ('Nyktis/iig-port', setup.IIG_PORT_COMMIT, 'NOTICE', 'IIG-NOTICE'),
               ('swiftlang/swift-corelibs-libdispatch', setup.LIBDISPATCH_REF, 'LICENSE', 'libdispatch-LICENSE'),
               ('lzfse/lzfse', setup.LZFSE_COMMIT, 'LICENSE', 'LZFSE-LICENSE')]
    provenance = []
    for repository, revision, path, name in sources:
        url = f'https://raw.githubusercontent.com/{repository}/{revision}/{path}'
        with urlopen(url, timeout=90) as response:
            data = response.read(1024**2 + 1)
        if not data or len(data) > 1024**2:
            raise ValueError(f'Unexpected license size: {name}')
        (directory / name).write_bytes(data)
        provenance.append({'url': url, 'sha256': hashlib.sha256(data).hexdigest()})
    # Carry corresponding cctools sources with its GPL-licensed binaries.
    url = f'https://codeload.github.com/tpoechtrager/cctools-port/tar.gz/{setup.CCTOOLS_PORT_REF}'
    with urlopen(url, timeout=90) as response:
        data = response.read(64 * 1024**2 + 1)
    if not data or len(data) > 64 * 1024**2:
        raise ValueError('Unexpected cctools source archive size')
    (directory / 'cctools-port-sources.tar.gz').write_bytes(data)
    provenance.append({'url': url, 'sha256': hashlib.sha256(data).hexdigest()})
    shutil.copyfile(tree / 'LICENSE', directory / 'UniXNU-LICENSE')
    shutil.copytree(tree / 'scripts/linux', directory / 'recipe', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__'))
    return provenance


def package(tree, prefix, output, tag):
    qualify(tree, prefix)
    if not re.fullmatch(r'picasso-linux-toolchain-v[1-9][0-9]*-ubuntu24-x86_64', tag):
        raise ValueError('Use a new numbered Ubuntu24 x86_64 toolchain tag')
    notices = add_notices(tree, prefix)
    output.mkdir(parents=True, exist_ok=False)
    files, links = inventory(prefix)
    archive = output / ARCHIVE
    with archive.open('xb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w|') as bundle:
            for path in (prefix, *sorted(prefix.rglob('*'))):
                info = bundle.gettarinfo(str(path), 'unixnu-linux/' + path.relative_to(prefix).as_posix())
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                if info.isfile():
                    with path.open('rb') as content:
                        bundle.addfile(info, content)
                else:
                    bundle.addfile(info)
    manifest = dict(schema=SCHEMA, tag=tag, archive=ARCHIVE, archive_sha256=digest(archive),
                    archive_bytes=archive.stat().st_size, platform='ubuntu-24.04-x86_64',
                    source_revision=run(['git', '-c', f'safe.directory={Path.cwd()}', 'rev-parse', 'HEAD'], text=True).strip(),
                    producer_run_id=os.environ.get('GITHUB_RUN_ID'),
                    repository=os.environ.get('GITHUB_REPOSITORY', 'YinCQL/UniXNU-picasso'),
                    upstream_revision=run(['git', '-c', f'safe.directory={tree.resolve()}', '-C', tree, 'rev-parse', 'HEAD'], text=True).strip(),
                    files=files, symlinks=links, notices=notices, **identity(tree, prefix))
    (output / MANIFEST).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    pin = dict(schema=SCHEMA, repository=manifest['repository'], tag=tag, archive=ARCHIVE,
               archive_sha256=manifest['archive_sha256'], manifest=MANIFEST,
               manifest_sha256=digest(output / MANIFEST), recipe_sha256=manifest['recipe_sha256'])
    (output / 'linux-toolchain-release.json').write_text(json.dumps(pin, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as result:
            result.write(f'manifest_sha256={pin["manifest_sha256"]}\ntag={tag}\n')
    print(json.dumps(pin, indent=2), flush=True)


def checked_manifest(bundle, manifest_sha):
    path = bundle / MANIFEST
    if not re.fullmatch('[0-9a-f]{64}', manifest_sha) or path.stat().st_size > 4 * 1024**2 or digest(path) != manifest_sha:
        raise ValueError('Pinned toolchain manifest digest mismatch')
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest['schema'] != SCHEMA or manifest['archive'] != ARCHIVE or manifest['platform'] != 'ubuntu-24.04-x86_64':
        raise ValueError('Unexpected toolchain manifest format')
    archive = bundle / ARCHIVE
    if not 0 < archive.stat().st_size <= 2 * 1024**3 or archive.stat().st_size != manifest['archive_bytes'] or digest(archive) != manifest['archive_sha256']:
        raise ValueError('Pinned toolchain archive digest/size mismatch')
    return manifest


def validate_archive(bundle):
    members = bundle.getmembers()
    names, total = set(), 0
    if not 0 < len(members) <= 50000:
        raise ValueError('Unexpected toolchain archive member count')
    for item in members:
        path = PurePosixPath(item.name)
        if '\\' in item.name or path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'unixnu-linux' or path.as_posix() in names:
            raise ValueError('Unsafe or duplicate toolchain archive path')
        names.add(path.as_posix())
        if not (item.isdir() or item.isfile() or item.issym() or item.islnk()):
            raise ValueError('Unsupported toolchain archive member')
        total += item.size
        if total > 6 * 1024**3:
            raise ValueError('Toolchain archive exceeds its unpacked size bound')
    # Python's data filter additionally resolves hard/symbolic links and refuses
    # links outside the destination, devices and unsafe file permissions.
    return members


def install(tree, bundle, manifest_sha, pin=None):
    require_platform()
    manifest = checked_manifest(bundle, manifest_sha)
    if manifest['prefix'] != str(PREFIX) or manifest['recipe_sha256'] != identity(tree)['recipe_sha256']:
        raise ValueError('Toolchain Release recipe, host LLVM or install prefix differs')
    if pin and (manifest['tag'] != pin['tag'] or manifest['repository'] != pin['repository'] or
                manifest['archive_sha256'] != pin['archive_sha256'] or manifest['recipe_sha256'] != pin['recipe_sha256']):
        raise ValueError('Toolchain Release differs from the committed pin')
    if os.geteuid() or (PREFIX.exists() and any(PREFIX.iterdir())):
        raise ValueError('Install as root into an empty fixed toolchain directory')
    with tarfile.open(bundle / ARCHIVE, 'r:gz') as archive:
        members = validate_archive(archive)
        archive.extractall(PREFIX.parent, members=members, filter='data')
    files, links = inventory(PREFIX)
    if files != manifest['files'] or links != manifest['symlinks']:
        raise ValueError('Unpacked toolchain contents differ from the manifest')
    qualify(tree, PREFIX)
    print(f'Installed pinned toolchain Release {manifest["tag"]} without source compilation', flush=True)


def read_pin(path):
    pin = json.loads(path.read_text(encoding='utf-8'))
    if pin['schema'] != SCHEMA or pin['archive'] != ARCHIVE or pin['manifest'] != MANIFEST:
        raise ValueError('Unexpected toolchain Release pin')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', pin['repository']) or not re.fullmatch(r'picasso-linux-toolchain-v[1-9][0-9]*-ubuntu24-x86_64', pin['tag']):
        raise ValueError('Invalid toolchain Release repository/tag')
    for field in ('archive_sha256', 'manifest_sha256', 'recipe_sha256'):
        if not re.fullmatch('[0-9a-f]{64}', pin[field]):
            raise ValueError(f'Invalid pinned digest: {field}')
    return pin


def download(pin_path, output):
    pin = read_pin(pin_path)
    output.mkdir(parents=True, exist_ok=False)
    run(['gh', 'release', 'download', pin['tag'], '--repo', pin['repository'],
         '--pattern', ARCHIVE, '--pattern', MANIFEST, '--dir', output])
    manifest = checked_manifest(output, pin['manifest_sha256'])
    if manifest['archive_sha256'] != pin['archive_sha256']:
        raise ValueError('Downloaded toolchain differs from the committed archive pin')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('identity', 'package', 'install'):
        p = commands.add_parser(name)
        p.add_argument('--tree', type=Path, required=True)
        if name == 'package':
            p.add_argument('--prefix', type=Path, default=PREFIX)
            p.add_argument('--output', type=Path, required=True)
            p.add_argument('--tag', required=True)
        if name == 'install':
            p.add_argument('--bundle', type=Path, required=True)
            group = p.add_mutually_exclusive_group(required=True)
            group.add_argument('--manifest-sha256')
            group.add_argument('--pin', type=Path)
    p = commands.add_parser('download')
    p.add_argument('--pin', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'identity':
        print('unixnu-toolchain-v1-Linux-X64-ubuntu24-' + identity(args.tree)['recipe_sha256'])
    elif args.command == 'package':
        package(args.tree, args.prefix, args.output, args.tag)
    elif args.command == 'install':
        pin = read_pin(args.pin) if args.pin else None
        install(args.tree, args.bundle, pin['manifest_sha256'] if pin else args.manifest_sha256, pin)
    else:
        download(args.pin, args.output)


if __name__ == '__main__':
    main()
