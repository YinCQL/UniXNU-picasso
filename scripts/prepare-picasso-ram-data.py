#!/usr/bin/env python3
"""Prepare a volatile APFS Data seed from the unchanged 21H461 OS (macOS/root).

Copies original private/var, adds empty runtime directories and the board's
explicit offline launch policy. The original OS remains mounted read-only.
All output, including failed preparations and mountpoint directories, is retained.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import plistlib
import runpy
import shutil
import stat
import struct
import subprocess
import sys
import tomllib
import uuid


# build81 exhausted APFS transaction headroom in the 256 MiB logical volume.
# Sparse pages still allocate only on writes; this is not a RAM reservation.
DATA_BYTES = 512 * 1024**2
PAGE = 4096
MAX_PACKED = 4 * 1024**2
MAX_LOGICAL = 2 * 1024**3  # Larger disks select the persistent-store driver path.
OS_SHA256 = '3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d'
LAUNCHD_SHA256 = '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25'
DATA_UUID_KEY = 'CONFIG_XNU_OWNER_DATA_VOLUME_UUID'
OFFLINE_POLICY_DIR = 'db/com.apple.xpc.launchd'
DIRECTORIES = (
    'db/dslocal/nodes/Default/users', 'db/dslocal/nodes/Default/groups',
    'db/dslocal/nodes/Default/config', 'containers/Bundle',
    'containers/Data/System', 'containers/Data/Application',
    'containers/Shared/SystemGroup', 'containers/Temp', 'PersonaVolumes',
    'preboot', 'root', 'tmp', 'run', 'Library', 'keybags/backup',
    'mobile/Library/Preferences', 'mobile/Library/Caches', 'mobile/Library/Logs',
    'mobile/Containers', 'mobile/Documents', 'mobile/Media', 'mobile/tmp',
)
ORIGINAL_PATHS = (
    'Library', 'Library/Preferences', 'private/var', 'var',
    'usr/share/zoneinfo.default', 'private/var/db/timezone',
)


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical_uuid(value):
    identity = uuid.UUID(value)
    if identity.int == 0:
        raise ValueError('Data volume UUID must not be zero')
    return str(identity).upper()


def configured_text(text, identity):
    """Change only one existing Data UUID setting after successful preparation."""
    identity = canonical_uuid(identity)
    lines = text.splitlines()
    for name in ('XNU_NATIVE_RAM_BLOCK', 'XNU_OWNER_DATA_MOUNT', 'XNU_OWNER_BOOT_MANIFEST'):
        if lines.count(f'CONFIG_{name}=y') != 1:
            raise ValueError(f'Require exactly one enabled CONFIG_{name}')
    matches = [i for i, line in enumerate(lines) if line.startswith(DATA_UUID_KEY + '=')]
    if len(matches) != 1:
        raise ValueError('Require exactly one existing Data UUID setting in config')
    lines[matches[0]] = f'{DATA_UUID_KEY}="{identity}"'
    return '\n'.join(lines) + '\n'


def metadata(path):
    info = path.lstat()
    value = dict(uid=info.st_uid, gid=info.st_gid, mode=stat.S_IMODE(info.st_mode))
    if stat.S_ISLNK(info.st_mode):
        value.update(type='symlink', target=os.readlink(path))
    elif stat.S_ISDIR(info.st_mode):
        value.update(type='directory')
    elif stat.S_ISREG(info.st_mode):
        value.update(type='file', bytes=info.st_size, sha256=sha256(path))
    else:
        raise ValueError(f'Unsupported source object: {path}')
    return value


def audit_path(root, relative):
    """Inspect a path inside a mount without following an intermediate symlink.

    Original iOS links may be absolute; following them on macOS would inspect
    the host instead of the source volume. Report that link and the unresolved
    suffix rather than inventing a mapping for it.
    """
    parts = Path(relative).parts
    if (not parts or Path(relative).is_absolute() or Path(relative).drive or
            any(part in ('.', '..') for part in parts)):
        raise ValueError('Audit path must be a normal relative mount path')
    current = root
    for index, part in enumerate(parts):
        current = current / part
        if not os.path.lexists(current):
            return {'type': 'missing', 'at': '/' + '/'.join(parts[:index + 1])}
        entry = metadata(current)
        if index != len(parts) - 1 and entry['type'] != 'directory':
            return dict(type='blocked', at='/' + '/'.join(parts[:index + 1]),
                        object=entry, unresolved='/'.join(parts[index + 1:]))
    return entry


def inventory(root):
    """Inventory without traversing symlinks or staged_system_apps."""
    result = {}

    def visit(path, relative):
        entry = metadata(path)
        result[relative] = entry
        if entry['type'] == 'directory':
            for child in sorted(path.iterdir()):
                if relative == '.' and child.name == 'staged_system_apps':
                    continue
                visit(child, child.name if relative == '.' else relative + '/' + child.name)

    visit(root, '.')
    if result['.']['type'] != 'directory':
        raise ValueError('The original private/var must be a real directory')
    return result


def copy_var(source, target, manifest):
    """Copy each inventoried object, then verify contents and POSIX metadata."""
    directories = []
    for relative, expected in manifest.items():
        src, dst = source / relative, target / relative
        kind = expected['type']
        if kind == 'directory':
            if relative != '.':
                dst.mkdir(mode=0o700, exist_ok=False)
            elif not dst.is_dir() or dst.is_symlink():
                raise ValueError('Destination must be the mounted Data root')
            directories.append((dst, expected))
            continue
        if os.path.lexists(dst):
            raise ValueError(f'Refuse to overwrite a formatted-volume object: {dst}')
        if kind == 'symlink':
            os.symlink(expected['target'], dst)
        else:
            # The mounted source is read-only and is inventoried again below.
            shutil.copy2(src, dst, follow_symlinks=False)
        os.chown(dst, expected['uid'], expected['gid'], follow_symlinks=False)
        if kind != 'symlink':
            os.chmod(dst, expected['mode'])
        elif stat.S_IMODE(dst.lstat().st_mode) != expected['mode']:
            # macOS supports lchmod; preserve a non-default source link mode too.
            os.lchmod(dst, expected['mode'])
    # Apply directory modes last so read-only source directories can be populated.
    for dst, expected in reversed(directories):
        os.chown(dst, expected['uid'], expected['gid'], follow_symlinks=False)
        os.chmod(dst, expected['mode'])
    verify_var(target, manifest)


def verify_var(target, manifest):
    for relative, expected in manifest.items():
        if metadata(target / relative) != expected:
            raise ValueError(f'Copied Data contents/ownership differ: {relative}')


def add_directories(target):
    """Add missing empty directories; never traverse a symlink or edit an object."""
    created = []
    for relative in DIRECTORIES:
        current = target
        for name in Path(relative).parts:
            parent = current
            current = current / name
            if os.path.lexists(current):
                if not stat.S_ISDIR(current.lstat().st_mode):
                    raise ValueError(f'Runtime directory is not a real directory: {current}')
                continue
            owner = parent.stat()
            rel = current.relative_to(target).as_posix()
            mode = 0o1777 if rel == 'tmp' else 0o700 if rel == 'root' else 0o755
            current.mkdir(mode=mode)
            os.chown(current, owner.st_uid, owner.st_gid)
            os.chmod(current, mode)
            actual = metadata(current)
            if actual != dict(type='directory', uid=owner.st_uid, gid=owner.st_gid, mode=mode):
                raise ValueError(f'New runtime directory ownership/mode mismatch: {rel}')
            created.append(dict(path=rel, **actual))
    return created


def offline_jobs(payload):
    """Only the unavailable telephony job may be excluded on volatile Data."""
    jobs = payload.get('offline_disabled_jobs', [])
    if not isinstance(jobs, list) or jobs not in ([], ['com.apple.CommCenter']):
        raise ValueError('Offline policy permits only com.apple.CommCenter once')
    adt = payload.get('adt', {})
    if jobs and (adt.get('packed_data_ramdisk') is not True or adt.get('persistent_data')):
        raise ValueError('Offline launch policy requires packed volatile Data')
    return jobs


def offline_policy_contents(jobs):
    if jobs not in ([], ['com.apple.CommCenter']):
        raise ValueError('Unexpected offline job list')
    if not jobs:
        return {}
    return {
        'disabled.plist': plistlib.dumps({job: True for job in jobs}, fmt=plistlib.FMT_XML),
        # The original 21H461 loader uses this marker to select its modern
        # override database instead of migrating the legacy overrides.
        'disabled.migrated': b'',
    }


def offline_policy_directory(target):
    current = target
    for part in ('', *Path(OFFLINE_POLICY_DIR).parts):
        if part:
            current = current / part
        entry = metadata(current)
        if (entry['type'] != 'directory' or entry['uid'] != 0 or entry['gid'] != 0
                or entry['mode'] & 0o022):
            raise ValueError('Offline policy requires real root-owned protected directories')
    return current


def verify_offline_policy(target, jobs):
    report = dict(disabled_jobs=jobs, watchdogd_disabled=False, files=[])
    contents = offline_policy_contents(jobs)
    if not contents:
        return report
    current = offline_policy_directory(target)
    for name, blob in contents.items():
        path = current / name
        expected = dict(type='file', uid=0, gid=0, mode=0o644,
                        bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
        # Check lstat metadata before reading to avoid following a symlink.
        if metadata(path) != expected or path.read_bytes() != blob:
            raise ValueError('Offline policy content/ownership readback mismatch')
        report['files'].append(dict(path=path.relative_to(target).as_posix(), **expected))
    return report


def install_offline_policy(target, jobs):
    """Create new root-owned overrides without editing any copied OS object."""
    contents = offline_policy_contents(jobs)
    if not contents:
        return verify_offline_policy(target, jobs)
    current = offline_policy_directory(target)
    for name in contents:
        if os.path.lexists(current / name):
            raise ValueError('Offline policy refuses existing objects: ' + name)
    for name, blob in contents.items():
        path = current / name
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0)
        fd = os.open(path, flags, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            if stream.write(blob) != len(blob):
                raise ValueError('Short offline policy write')
            stream.flush()
            os.fsync(stream.fileno())
        os.chown(path, 0, 0, follow_symlinks=False)
        os.chmod(path, 0o644)
    return verify_offline_policy(target, jobs)


def inspect_packed(path, raw=None):
    """Validate DBRDISK1 and hash its reconstruction, optionally checking raw bytes.

    The 4 MiB input cap reserves space in Picasso's 60 MiB FAT payload. Logical
    media above 2 GiB is rejected because the pinned driver needs persistent I/O.
    """
    path = Path(path)
    size = path.stat().st_size
    if not 32 <= size <= MAX_PACKED:
        raise ValueError('Packed Data must be between 32 bytes and 4 MiB')
    blob = path.read_bytes()
    magic, version, page, logical, count = struct.unpack_from('<8sIIQQ', blob)
    if (magic != b'DBRDISK1' or version != 1 or page != PAGE or
            not 0 < logical <= MAX_LOGICAL or logical % PAGE or
            count > 65536 or count > (size - 32) // 16):
        raise ValueError('Invalid packed Data header or volatile logical capacity')
    cursor, end = 32 + 16 * count, 0
    extents = []
    for index in range(count):
        offset, length = struct.unpack_from('<QQ', blob, 32 + 16 * index)
        if (not length or offset % PAGE or length % PAGE or offset < end or
                offset > logical or length > logical - offset or length > size - cursor):
            raise ValueError('Invalid packed Data extent')
        extents.append((offset, memoryview(blob)[cursor:cursor + length]))
        end, cursor = offset + length, cursor + length
    if cursor != size:
        raise ValueError('Trailing packed Data bytes')
    digest = hashlib.sha256()
    raw_stream = None
    if raw is not None:
        raw = Path(raw)
        if raw.stat().st_size != logical:
            raise ValueError('Raw Data length differs from packed logical capacity')
        raw_stream = raw.open('rb')
    zero, end = bytes(1024**2), 0

    def consume(data):
        digest.update(data)
        if raw_stream is not None and raw_stream.read(len(data)) != data:
            raise ValueError('Packed reconstruction differs from raw Data bytes')

    try:
        for offset, data in [*extents, (logical, b'')]:
            while end < offset:
                amount = min(len(zero), offset - end)
                consume(memoryview(zero)[:amount])
                end += amount
            consume(data)
            end += len(data)
    finally:
        if raw_stream is not None:
            raw_stream.close()
    return dict(logical_bytes=logical, packed_bytes=size, raw_sha256=digest.hexdigest(),
                packed_sha256=hashlib.sha256(blob).hexdigest(), page_size=page,
                extent_count=count)


@contextmanager
def mounted(image, target, readonly=False):
    target.mkdir(parents=True, exist_ok=False)
    options = ['-readonly', '-noautofsck'] if readonly else []
    attached = plistlib.loads(subprocess.check_output([
        'hdiutil', 'attach', '-nobrowse', '-owners', 'on', *options,
        '-mountpoint', str(target), '-plist', str(image)]))
    entities = attached['system-entities']
    device = entities[0]['dev-entry']
    try:
        volumes = [entry for entry in entities if entry.get('mount-point') == str(target)]
        if len(volumes) != 1:
            raise ValueError('Require exactly one mounted APFS volume')
        info = plistlib.loads(subprocess.check_output([
            'diskutil', 'info', '-plist', volumes[0]['dev-entry']]))
        # Query the mounted filesystem itself. diskutil's plist read-only keys
        # differ from its human-readable labels; a missing key is not evidence
        # of a writable mount. ST_RDONLY is the POSIX statvfs mount flag.
        flags = os.statvfs(target).f_flag
        actual_readonly = bool(flags & os.ST_RDONLY)
        audit = dict(image=str(image), mountpoint=str(target),
                     requested_readonly=readonly, actual_readonly=actual_readonly,
                     statvfs_flags=flags, diskutil=info)
        # Save outside the mount so failure diagnostics survive detach and are
        # included by the workflows' existing ram-data/*.json artifact pattern.
        target.with_name(target.name + '-mount.json').write_text(
            json.dumps(audit, indent=2, default=str) + '\n', encoding='utf-8')
        print(f'APFS_MOUNT path={target} readonly={int(actual_readonly)} '
              f'expected={int(readonly)} flags={flags:#x} '
              f'owners={info.get("GlobalPermissionsEnabled")}', flush=True)
        if info.get('FilesystemType') != 'apfs':
            raise ValueError('Require an APFS filesystem')
        if actual_readonly != readonly:
            raise ValueError(f'Mounted volume has unexpected read-only state: '
                             f'expected={readonly}, actual={actual_readonly}, flags={flags:#x}')
        if info.get('GlobalPermissionsEnabled') is not True:
            raise ValueError('Mounted volume must have ownership enabled')
        yield info
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)


def prepare(tree, source, output, config):
    if sys.platform != 'darwin' or os.geteuid() != 0:
        raise ValueError('APFS Data preparation requires macOS and root')
    if not source.is_file() or output.exists():
        raise ValueError('Require an original OS image and a new output directory')
    original_config = config.read_text(encoding='utf-8')
    # Validate its shape without changing it before touching any output.
    configured_text(original_config, '11111111-1111-4111-8111-111111111111')
    board = tomllib.loads((Path(__file__).resolve().parents[1] / 'board.toml').read_text(encoding='utf-8'))
    jobs = offline_jobs(board['payload'])
    packer_path = tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'
    pack = runpy.run_path(str(packer_path))['pack']
    if sha256(source) != OS_SHA256:
        raise ValueError('Original 21H461 OS image SHA256 mismatch')
    output.mkdir(parents=True, exist_ok=False)
    image = output / 'data.cdr'
    packed = output / 'data.dbrdisk'
    original, target = output / 'os', output / 'data'
    with mounted(source, original, readonly=True):
        launchd = audit_path(original, 'sbin/launchd')
        if launchd.get('type') != 'file' or launchd['sha256'] != LAUNCHD_SHA256:
            raise ValueError('Original launchd identity mismatch')
        source_var = original / 'private/var'
        if audit_path(original, 'private/var').get('type') != 'directory':
            raise ValueError('Original private/var must not traverse a symlink')
        manifest = inventory(source_var)
        source_paths = {'/' + relative: audit_path(original, relative) for relative in ORIGINAL_PATHS}
        subprocess.run([
            'hdiutil', 'create', '-size', f'{DATA_BYTES // 1024**2}m', '-layout', 'NONE',
            '-fs', 'APFS', '-volname', 'OwnerData', '-type', 'UDTO', str(image)], check=True)
        with mounted(image, target) as info:
            identity = canonical_uuid(info['VolumeUUID'])
            copy_var(source_var, target, manifest)
            created = add_directories(target)
            verify_var(target, manifest)
            launch_policy = install_offline_policy(target, jobs)
            verify_var(target, manifest)
            if inventory(source_var) != manifest:
                raise ValueError('Original var changed during preparation')
    if sha256(source) != OS_SHA256:
        raise ValueError('Original OS image changed during preparation')
    with image.open('rb') as stream:
        header = stream.read(PAGE)
    if (image.stat().st_size != DATA_BYTES or header[32:36] != b'NXSB' or
            struct.unpack_from('<I', header, 36)[0] != PAGE or
            struct.unpack_from('<Q', header, 40)[0] * PAGE != DATA_BYTES):
        raise ValueError('Host formatting did not produce the requested raw APFS geometry')
    pack(image, packed)
    packed_info = inspect_packed(packed, raw=image)
    if packed_info['logical_bytes'] != DATA_BYTES:
        raise ValueError('Expected exactly 512 MiB of logical volatile Data')
    manifest_path = output / 'source-var-manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    files = [entry for entry in manifest.values() if entry['type'] == 'file']
    report = dict(
        schema='picasso-ram-data-v1', volume_uuid=identity, **packed_info,
        source_sha256=OS_SHA256, source_launchd_sha256=LAUNCHD_SHA256,
        source_regular_files=len(files), source_regular_bytes=sum(entry['bytes'] for entry in files),
        source_manifest_sha256=sha256(manifest_path), original_paths=source_paths,
        created_directories=created, packer_sha256=sha256(packer_path),
        offline_launch_policy=launch_policy,
        guest_mutable=True, persistent=False, ownership_preserved=True,
        os_inputs_modified=False, hardware_tested=False,
        excluded=['private/var/staged_system_apps'],
    )
    (output / 'seed.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    # Both mounted images have detached and raw/packed bytes have been verified.
    # Retain the previous config as an audit input and update its UUID last.
    if config.read_text(encoding='utf-8') != original_config:
        raise ValueError('Config changed during Data preparation')
    (output / 'config-before.txt').write_text(original_config, encoding='utf-8')
    config.write_text(configured_text(original_config, identity), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('tree', 'source', 'output', 'config'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    prepare(*(getattr(args, name).resolve() for name in ('tree', 'source', 'output', 'config')))
