#!/usr/bin/env python3
"""Plan separate super root and userdata APFS backing without device writes."""
import argparse
import json
from pathlib import Path
from decimal import Decimal, InvalidOperation

BLOCK = 4096
GIB_BLOCKS = 2**30 // BLOCK
GUARD_BLOCKS = 1024
AUX_BLOCKS = 65536
RESERVE_BLOCKS = 262144
MAX_GIB = 1024


def gib_to_mib(gib):
    try:
        value = Decimal(str(gib)) * 1024
    except (InvalidOperation, ValueError):
        raise ValueError('Invalid GiB capacity') from None
    if not value.is_finite() or value != value.to_integral_value():
        raise ValueError('GiB capacity must be an exact multiple of 1 MiB')
    return int(value)


def window_blocks_mib(mib):
    pages = mib * 256
    return pages + (pages + 32767) // 32768 + 1


def window_blocks(gib):
    return window_blocks_mib(gib_to_mib(gib))


def plan(userdata_bytes, data_gib=None, first_lba=0, data_mib=None):
    if type(userdata_bytes) is not int or userdata_bytes <= 0 or userdata_bytes % BLOCK:
        raise ValueError('userdata bytes must be a measured positive multiple of 4096')
    if type(first_lba) is not int or first_lba < 0:
        raise ValueError('userdata first LBA must be nonnegative')
    partition = userdata_bytes // BLOCK
    if first_lba + partition > 0xffffffff:
        raise ValueError('userdata exceeds the driver READ/WRITE(10) address range')
    available = partition - GUARD_BLOCKS - AUX_BLOCKS - RESERVE_BLOCKS
    maximum = min(MAX_GIB*1024, max(0, available // 256))
    while maximum >= 4096 and window_blocks_mib(maximum) > available:
        maximum -= 1
    if maximum < 4096:
        raise ValueError('userdata cannot fit 4 GiB Data and required metadata/reserves')
    if data_gib is not None and data_mib is not None:
        raise ValueError('Specify only one of data_gib or data_mib')
    chosen = gib_to_mib(data_gib) if data_gib is not None else data_mib
    chosen = maximum if chosen is None else chosen
    if type(chosen) is not int or not 4096 <= chosen <= maximum:
        raise ValueError(f'Data size must be an integer between 4096 and {maximum} MiB for this partition')
    blocks = window_blocks_mib(chosen)
    data_first = partition - RESERVE_BLOCKS - blocks
    aux_first = data_first - AUX_BLOCKS
    regions = {
        'guard': (0, GUARD_BLOCKS),
        'aux': (aux_first, AUX_BLOCKS),
        'data_overlay': (data_first, blocks),
        'tail_reserve': (partition - RESERVE_BLOCKS, RESERVE_BLOCKS),
        'nvram': (partition - 8192 - 65536, 6),
        'aux_owner': (partition - 8192 - 65536 + 6, 4),
        'init_journal': (partition - 8192 - 65536 + 10, 4),
        'console': (partition - 8192, 130),
    }
    return {
        'schema': 'picasso-super-data-layout-v1',
        'root_partition': 'super',
        'data_partition': 'userdata',
        'userdata_bytes': userdata_bytes,
        'userdata_first_lba': first_lba,
        'userdata_last_lba': first_lba + partition - 1,
        'block_bytes': BLOCK,
        'maximum_data_gib': maximum // 1024,
        'maximum_data_mib': maximum,
        'data_gib': chosen / 1024,
        'data_mib': chosen,
        'data_bytes': chosen * 2**20,
        'data_pages': chosen * 256,
        'unused_before_aux_bytes': (aux_first - GUARD_BLOCKS) * BLOCK,
        'regions': {name: {'first_lba': first_lba + start, 'blocks': count,
                           'last_lba': first_lba + start + count - 1,
                           'relative_offset': start * BLOCK, 'bytes': count * BLOCK}
                    for name, (start, count) in regions.items()},
        'requires_matching_apfs_seed': True,
        'apfs_seed_generation': 'Native macOS hdiutil/diskutil; size must equal data_bytes',
        'capacity_source': 'caller-supplied measured userdata geometry; retail SKU is not sufficient',
        'device_written': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--userdata-bytes', type=int, required=True,
                        help='Measured physical userdata partition bytes')
    parser.add_argument('--userdata-first-lba', type=int, default=0)
    parser.add_argument('--data-gib', help='GiB capacity, exact multiples of 1 MiB')
    parser.add_argument('--data-mib', type=int, help='MiB capacity; omit both size options to fill available Data space')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = plan(args.userdata_bytes, args.data_gib, args.userdata_first_lba, args.data_mib)
    content = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as stream:
            stream.write(content)
    print(content, end='')


if __name__ == '__main__':
    main()
