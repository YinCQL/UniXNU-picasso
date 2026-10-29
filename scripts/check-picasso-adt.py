#!/usr/bin/env python3
"""Generate and decode picasso ADTs without building a payload or accessing a phone.

Keep all generated trees in --output. This checks the board-to-driver ABI,
including property placement, width and byte order; it cannot prove IRQ delivery.
"""
import argparse
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
    results = []
    for name, extra in [('minimal', []), ('board', board_args),
                        ('board-with-data', board_args + ['--packed-data-ramdisk'])]:
        target = out / (name + '.adt')
        subprocess.run([sys.executable, str(args.generator.resolve()), '--output', str(target),
                        '--dram-size', str(board['dram_size']), *extra], check=True)
        data = target.read_bytes()
        nodes = decode(data)
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
                        'timer_intid': 19, 'cpu_nodes': len(cpu_nodes)})
    report = {'scope': 'generated ADT ABI only; no payload build or hardware IRQ test',
              'generator_sha256': hashlib.sha256(args.generator.read_bytes()).hexdigest(),
              'cases': results}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: picasso ADT timer INTID, encoding, GIC layout and CPU timebase')


if __name__ == '__main__':
    main()
