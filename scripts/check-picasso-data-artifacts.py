#!/usr/bin/env python3
"""Check the actual compiled RAM Data provider and FAT handoff; keep audit files.

Run on the macOS build host after kernel + payload. This does not establish that
the device has booted or that its later Data writes fit in physical memory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import subprocess
import tomllib
import uuid

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def check(work, tree, output):
    board = tomllib.loads((ROOT / 'board.toml').read_text(encoding='utf-8'))
    spec = board['payload']
    require(spec['size_mib'] == 60, 'Picasso payload must be 60 MiB')
    require(spec['adt'].get('packed_data_ramdisk') is True, 'Packed Data ADT mode required')
    require(not spec['adt'].get('persistent_data'), 'Data must remain volatile')
    for argument in spec['kernel_args']:
        require(argument.split('=')[0] not in ('sheng-data-persist', 'sheng-log'),
                'Persistent UFS store must remain disabled')

    seed_dir = work / 'ram-data'
    seed = json.loads((seed_dir / 'seed.json').read_text())
    inspect = runpy.run_path(str(ROOT / 'scripts/prepare-picasso-ram-data.py'))['inspect_packed']
    packed = inspect(seed_dir / 'data.dbrdisk')
    require(seed['schema'] == 'picasso-ram-data-v1', 'Unknown Data report')
    for key in ('logical_bytes', 'packed_bytes', 'packed_sha256', 'raw_sha256',
                'page_size', 'extent_count'):
        require(seed[key] == packed[key], f'Data report mismatch: {key}')
    require(packed['logical_bytes'] == 256 * 1024**2, 'Expected 256 MiB sparse logical disk')
    require(packed['packed_bytes'] <= 4 * 1024**2, 'Packed seed exceeds its memory/payload budget')
    require(seed['persistent'] is False and seed['ownership_preserved'] is True
            and seed['os_inputs_modified'] is False, 'Unsafe seed provenance')
    require(seed['source_launchd_sha256'] ==
            '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25',
            'Unexpected source OS')
    require(seed['source_sha256'] ==
            '3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d',
            'Unexpected source OS image hash')
    require(digest(seed_dir / 'source-var-manifest.json') == seed['source_manifest_sha256'],
            'Source manifest hash mismatch')
    identity = str(uuid.UUID(seed['volume_uuid'])).upper()
    require(uuid.UUID(identity).int != 0, 'Zero Data UUID')
    config = (work / '.config').read_text()
    for name in ('NATIVE_UFS', 'NATIVE_RAM_BLOCK', 'OWNER_DATA_MOUNT', 'OWNER_BOOT_MANIFEST'):
        require(config.splitlines().count(f'CONFIG_XNU_{name}=y') == 1, f'Missing {name}')
    require(config.splitlines().count(f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{identity}"') == 1,
            'Config does not select this Data seed')
    header = (tree / 'build/xnu/owner-data-volume.h').read_text()
    require(f'#define XNU_OWNER_DATA_VOLUME_UUID "{identity}"' in header,
            'Compiled Data UUID header mismatch')

    kernel = work / 'kernel/kernel.macho'
    plan = json.loads(kernel.with_suffix('.json').read_text())
    require(digest(kernel) == plan['candidate_sha256'], 'Prelink report kernel hash mismatch')
    require('com.apple.iokit.IOStorageFamily' in plan['components'], 'Missing original IOStorage')
    image = kernel.read_bytes()
    require(identity.encode() + b'\0' in image, 'Kernel lacks the actual Data volume UUID')
    require(b"'IOMatchCategory' = NativeRamBlock;" in image, 'Missing independent RAM match category')
    symbols = {}
    for line in subprocess.check_output(['llvm-nm', '--defined-only', str(kernel)]).decode().splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    require('_native_ram_block_volume_seed' in symbols, 'Kernel lacks the RAM volume provider')
    require('_native_ufs_store_open' in symbols, 'Kernel lacks its UFS provider')
    ram = tree / 'build/native-ram-block'
    ufs = tree / 'build/native-ufs'
    imports = json.loads((ram / 'storage-imports.json').read_text())
    for path, key in ((ram / 'NativeRamBlock.o', 'object_sha256'),
                      (tree / 'drivers/NativeRamBlock/NativeRamBlock.cpp', 'source_sha256'),
                      (ufs / 'storage-bindings.json', 'bindings_sha256'),
                      (ufs / 'storage-bindings.o', 'binding_object_sha256')):
        require(digest(path) == imports[key], f'Stale RAM ABI audit: {path.name}')
    require(int(imports['slide'], 0) == int(board['kernel']['prelink']['slide'], 0),
            'RAM storage slide mismatch')
    require(bool(imports['imports']), 'Empty RAM storage import audit')
    for name, target in imports['imports'].items():
        address = int(target, 0)
        require(symbols.get(name) == address, f'Wrong linked storage target: {name}')
        require(any(e['module'] == 'com.apple.iokit.IOStorageFamily'
                    and e['va'] <= address < e['va'] + e['size'] for e in plan['extents']),
                f'Storage target outside original component: {name}')

    payload = work / 'payload/payload.img'
    require(payload.stat().st_size == 60 * 1024**2, 'Incorrect FAT payload size')
    payload_report = json.loads(payload.with_suffix('.json').read_text())
    require(payload_report['payload_sha256'] == digest(payload), 'Stale payload report')
    require(payload_report['data_ramdisk_sha256'] == packed['packed_sha256'], 'Wrong payload seed')
    require(payload_report['kernel_args'] == ' '.join(spec['kernel_args']), 'Unexpected boot arguments')
    extracted = output.with_suffix('.files')
    extracted.mkdir(parents=True, exist_ok=False)
    subprocess.run(['mcopy', '-i', str(payload), '-s', '::/EFI', str(extracted)], check=True)
    directory = extracted / 'EFI/UniXNU'
    files = directory / 'Payloads'
    require(digest(files / 'scratch-data.img') == packed['packed_sha256'], 'FAT Data bytes mismatch')
    require(digest(files / 'kernel.macho') == digest(kernel), 'FAT kernel bytes mismatch')
    require(digest(files / 'kernel.macho.trustcache') == payload_report['trust_cache_sha256'],
            'FAT trust cache bytes mismatch')
    entries = tomllib.loads((directory / 'unixnu.toml').read_text())['entry']
    entry = next(e for e in entries if e['id'] == 'xnu')
    require(entry['initrd'] == 'Payloads/scratch-data.img', 'Loader lacks Data initrd')
    require(entry['args'] == payload_report['kernel_args'], 'FAT boot arguments mismatch')
    adt_check = runpy.run_path(str(ROOT / 'scripts/check-picasso-adt.py'))
    nodes = adt_check['decode']((files / 'kernel.macho.adt').read_bytes())
    owned = adt_check['check_owned_memory'](nodes, int(str(spec['adt']['owned_memory_size']), 0))
    memory_map = nodes['/chosen/memory-map']
    require(memory_map.get('PackedRAMDisk') == bytes(16) and 'RAMDisk' not in memory_map,
            'ADT must use exactly one packed placeholder')
    require(nodes['/chosen'].get('ephemeral-storage') == struct.pack('<I', 1),
            'ADT does not declare ephemeral Data')
    content = sum(p.stat().st_size for p in extracted.rglob('*') if p.is_file())
    require(content * 100 <= payload.stat().st_size * 95, 'FAT content exceeds 95% budget')
    report = dict(scope='Actual build artifacts; no device execution or peak RAM qualification',
                  volume_uuid=identity, packed=packed, payload_bytes=payload.stat().st_size,
                  content_bytes=content, content_budget_bytes=payload.stat().st_size * 95 // 100,
                  kernel_sha256=digest(kernel), storage_imports=imports['imports'],
                  owned_memory_request_bytes=owned,
                  persistent=False, hardware_tested=False)
    output.write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.work.resolve(), args.tree.resolve(), args.output.resolve()), indent=2))
