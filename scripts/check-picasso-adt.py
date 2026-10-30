#!/usr/bin/env python3
"""Generate and decode picasso ADTs without building a payload or accessing a phone.

Keep all generated trees in --output. This checks the board-to-driver ABI,
including property placement, width and byte order; it cannot prove IRQ delivery.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def decode(data):
    nodes = {}

    def read_node(offset, parent):
        count, children = struct.unpack_from('<II', data, offset)
        offset += 8
        properties = {}
        for _ in range(count):
            key, length = struct.unpack_from('<32sI', data, offset)
            key = key.split(b'\0', 1)[0].decode('ascii')
            offset += 36
            assert key not in properties, f'duplicate property: {key}'
            assert offset + length <= len(data), 'truncated property'
            properties[key] = data[offset:offset + length]
            offset += (length + 3) & ~3
        name = properties['name'].rstrip(b'\0').decode('ascii')
        path = (parent + '/' + name) if parent is not None else ''
        assert path not in nodes, f'duplicate node: {path}'
        nodes[path] = properties
        for _ in range(children):
            offset = read_node(offset, path)
        return offset

    assert read_node(0, None) == len(data), 'trailing ADT bytes'
    return nodes


def check_owned_memory(nodes, requested):
    """Validate the actual loader request; this does not prove an allocation."""
    if not 0x10000000 <= requested <= 0x200000000 or requested % 0x200000:
        raise ValueError('Invalid DarwinBoot owned-memory request')
    key = 'darwinboot-memory-size'
    if (nodes['/chosen'].get(key) != struct.pack('<Q', requested)
            or sum(key in node for node in nodes.values()) != 1):
        raise ValueError('ADT owned-memory request missing, misplaced or different from board')
    return requested


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--generator', type=Path, default=ROOT / 'scripts/make-picasso-adt.py',
                        help='Alternate generator for regression comparison')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    board = tomllib.loads((ROOT / 'board.toml').read_text(encoding='utf-8'))['payload']
    adt = board['adt']
    assert adt['tool'] == 'scripts/make-picasso-adt.py'
    board_args = []
    for key in ('owned_memory_size', 'root_match_volume_uuid', 'chip_id', 'cpu_regs', 'psci_method'):
        if adt.get(key) is not None:
            board_args += ['--' + key.replace('_', '-'), str(adt[key])]
    # Synthetic, non-device identity seed for host-only generation.
    board_args += ['--unique-chip-id', '1', '--trust-cache']
    if adt.get('ipad_identity'):
        board_args += ['--ipad-identity']
    if adt.get('iphone_identity'):
        board_args += ['--iphone-identity']
    results = []
    for name, extra in [('minimal', []), ('board', board_args),
                        ('board-with-data', board_args + ['--packed-data-ramdisk'])]:
        target = out / (name + '.adt')
        subprocess.run([sys.executable, str(args.generator.resolve()), '--output', str(target),
                        '--dram-size', str(board['dram_size']), *extra], check=True)
        data = target.read_bytes()
        nodes = decode(data)
        owned = None
        if name != 'minimal':
            owned = check_owned_memory(nodes, int(str(adt['owned_memory_size']), 0))
        else:
            assert not any('darwinboot-memory-size' in node for node in nodes.values())
        memory_map = nodes['/chosen/memory-map']
        if name == 'board-with-data':
            assert memory_map.get('PackedRAMDisk') == bytes(16)
            assert 'RAMDisk' not in memory_map
            assert nodes['/chosen'].get('ephemeral-storage') == struct.pack('<I', 1)
        else:
            assert 'PackedRAMDisk' not in memory_map and 'RAMDisk' not in memory_map
            assert 'ephemeral-storage' not in nodes['/chosen']
        intc = nodes['/arm-io/intc']
        assert intc.get('virtual-timer-intid') == b'\x13\0\0\0', \
            'picasso requires /arm-io/intc/virtual-timer-intid as LE u32 19'
        assert sum('virtual-timer-intid' in node for node in nodes.values()) == 1
        assert intc['compatible'] == b'arm,gic-v3\0'
        assert struct.unpack('<4Q', intc['reg']) == (0, 0x10000, 0x60000, 0x100000)
        assert struct.unpack('<Q', intc['redistributor-stride']) == (0x40000,)
        cpu_nodes = [node for path, node in nodes.items() if path.startswith('/cpus/')]
        assert len(cpu_nodes) == (1 if name == 'minimal' else len(adt['cpu_regs'].split(',')))
        assert all(node['timebase-frequency'] == struct.pack('<I', 19200000) for node in cpu_nodes)
        results.append({'case': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                        'timer_intid': 19, 'cpu_nodes': len(cpu_nodes),
                        'owned_memory_request_bytes': owned})
    # Exercise the guard used on FAT-extracted bytes, including the old request
    # that would silently retain the undersized window if propagation broke.
    requested = int(str(adt['owned_memory_size']), 0)
    key = 'darwinboot-memory-size'
    rejected = []
    bad_values = [('missing', None), ('old-256MiB', struct.pack('<Q', 0x10000000)),
                  ('big-endian', struct.pack('>Q', requested)),
                  ('u32', struct.pack('<I', requested & 0xffffffff))]
    for case, value in [*bad_values, ('misplaced', None), ('duplicate', None)]:
        candidate = copy.deepcopy(nodes)
        if case in ('misplaced', 'duplicate'):
            candidate[''][key] = candidate['/chosen'][key]
        if case != 'duplicate':
            if value is None:
                candidate['/chosen'].pop(key)
            else:
                candidate['/chosen'][key] = value
        try:
            check_owned_memory(candidate, requested)
        except ValueError:
            rejected.append(case)
        else:
            raise AssertionError(f'Accepted incorrect memory request: {case}')
    report = {'scope': 'generated ADT ABI only; no payload build or hardware IRQ test',
              'generator_sha256': hashlib.sha256(args.generator.read_bytes()).hexdigest(),
              'cases': results, 'rejected_memory_handoffs': rejected}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: picasso ADT memory request, timer INTID, encoding, GIC layout and CPU timebase')


if __name__ == '__main__':
    main()
