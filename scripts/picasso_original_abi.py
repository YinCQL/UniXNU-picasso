"""Read qualified original ABI bytes without a previous native build candidate.

Addresses used by the board's ABI checks describe original components at the
fixed prelink slide. Read those bytes from the unmodified, hash-pinned IPSW
kernelcache; rebase pointer values explicitly, never instruction bytes.
"""
import hashlib
import os
from pathlib import Path
import struct
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import backed_offset, exports, macho

ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
SLIDE = -0x1000000


def require(value, message):
    if not value:
        raise SystemExit(message)


class OriginalReference:
    def __init__(self, path, board):
        prelink = board['kernel']['prelink']
        require(prelink['kernelcache_sha256'] == ORIGINAL_SHA256,
                'Unsupported original ABI kernelcache configuration')
        require(int(prelink['slide'], 0) == SLIDE, 'Unsupported original ABI slide')
        self.path = Path(path)
        self.data = self.path.read_bytes()
        require(hashlib.sha256(self.data).hexdigest() == ORIGINAL_SHA256,
                'Original ABI kernelcache SHA256 mismatch')
        self.image = macho(self.data)
        self.names = {name: address + SLIDE
                      for address, names in exports(self.data, self.image).items()
                      for name in names}

    def read(self, address, length):
        offset = backed_offset(self.image['segments'], address - SLIDE,
                               length, len(self.data))
        return self.data[offset:offset + length]

    def pointers(self, address, count):
        return tuple(value + SLIDE if value else 0
                     for value in struct.unpack(f'<{count}Q', self.read(address, count * 8)))


def load_reference():
    device = Path(os.environ.get('UNIXNU_DEVICE', ROOT))
    board = tomllib.loads((device / 'board.toml').read_text(encoding='utf-8'))
    path = os.environ.get('UNIXNU_KERNELCACHE')
    require(path, 'UNIXNU_KERNELCACHE must name the pinned original ABI kernelcache')
    return OriginalReference(path, board)


def qualify_legacy(reference):
    read, names = reference.read, reference.names

    def vptr(pc):
        a, b, c = struct.unpack('<3I', read(pc, 12))
        require(a & 0x9f00001f == 0x90000008 and b & 0xffc003ff == 0x91000108
                and c == 0x91004108, 'Original Legacy constructor vptr sequence')
        immediate = ((a >> 5) & 0x7ffff) * 4 + ((a >> 29) & 3)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        return (pc & ~4095) + (immediate << 12) + ((b >> 10) & 4095) + 16

    def branch(pc):
        word, = struct.unpack('<I', read(pc, 4))
        require(word & 0xfc000000 == 0x94000000, 'Original Legacy constructor BL')
        immediate = word & 0x3ffffff
        return pc + 4 * (immediate - (1 << 26) if immediate & (1 << 25) else immediate)

    base_vptr = vptr(0xfffffff00577b324)
    service_vptr = vptr(0xfffffff00577b51c)
    legacy_vptr = vptr(0xfffffff0057865d8)
    base = reference.pointers(base_vptr, 262)
    service = reference.pointers(service_vptr, 262)
    legacy = reference.pointers(legacy_vptr, 358)
    require(all(legacy) and read(legacy_vptr + 358 * 8, 24) == bytes(24),
            'Original Legacy vtable extent')
    pure = [i for i, address in enumerate(legacy) if address == names['___cxa_pure_virtual']]
    require(pure == [211, 213, 245, 246, 306, 307, 337, 338, 345],
            'Original Legacy abstract slots')
    require([i for i, pair in enumerate(zip(base, service)) if pair[0] != pair[1]] == [0, 1, 7],
            'Original Service inheritance')
    changed = [i for i, pair in enumerate(zip(base, legacy)) if pair[0] != pair[1]]
    require(len(changed) == 103 and [i for i in changed if i < 168] ==
            [0, 1, 7, 14, 21, 44, 79, 85, 86, 87, 111, 141, 155],
            'Original Legacy inheritance')
    require(base[141] == names['__ZN9IOService13newUserClientEP4taskPvjPP12IOUserClient'],
            'Original newUserClient ABI')
    require(branch(0xfffffff0057865fc) == 0xfffffff00577b510
            and branch(0xfffffff00577b540) == 0xfffffff00577b318
            and branch(0xfffffff005786630) == 0xfffffff00577b55c,
            'Original Legacy C2/D2 chain')
    require(legacy[295] == 0xfffffff005795b88 and legacy[301] == 0xfffffff00579ea5c,
            'Original completion and refresh slots')
    for offset, field in ((0x30, 5808), (0x58, 5812)):
        word, = struct.unpack('<I', read(legacy[249] + offset, 4))
        require(word == 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8,
                'Original display dimensions layout')
    return dict(original_sha256=ORIGINAL_SHA256, slide=SLIDE,
                reference='unmodified IPSW kernelcache', runtime_qualified=False,
                base_vptr=hex(base_vptr), service_vptr=hex(service_vptr), legacy_vptr=hex(legacy_vptr),
                legacy_instance_bytes=0x18a0, legacy_virtual_entries=358, abstract_slots=pure,
                methods=[dict(slot=i, address=hex(address)) for i, address in enumerate(legacy)])


def qualify_hid(reference):
    read, names = reference.read, reference.names
    vptr = 0xfffffff005d045c0
    methods = reference.pointers(vptr, 207)
    base = reference.pointers(names['__ZTV9IOService'] + 16, 168)
    require([i for i in range(168) if methods[i] != base[i]] ==
            [0, 1, 7, 12, 14, 21, 23, 76, 81, 82, 86, 87, 91, 92, 93, 101, 132, 140],
            'Exact original HID native-base overrides')
    require([i for i, address in enumerate(methods) if address == names['___cxa_pure_virtual']] == [180]
            and all(methods) and read(vptr + 207 * 8, 24) == bytes(24),
            'Exact original 207-slot abstract device')
    require(read(0xfffffff004ad94f8, 0x20) ==
            bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),
            'Original protected constructor and exact vptr store')
    require(reference.pointers(0xfffffff005d00f10, 1)[0] == names['__ZN9IOServiceC2EPK11OSMetaClass'],
            'Actual native protected constructor binding')
    require(reference.pointers(0xfffffff005d00f18, 1)[0] == names['__ZN9IOServiceD2Ev'],
            'Actual native protected destructor binding')
    require(read(0xfffffff004ad9aec, 0x34) ==
            bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'),
            'Original handleStart and report-descriptor ABI')
    require(methods[181] == 0xfffffff004add0c4 and read(0xfffffff004add110, 28) ==
            bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6'),
            'Original handleReport forwards timestamp, report, type and options')
    return methods


def qualify_hid_event_driver(reference):
    # IOHIDFamily-2031.120.4: constructors name the class and store these vptrs.
    # The instance table is 220 slots; the former 256-slot copy read past it.
    require(reference.read(0xfffffff004af09bc, 12) ==
            bytes.fromhex('a89000d008a12991080000f9'),
            'Original IOHIDEventDriver constructor vptr')
    require(reference.read(0xfffffff004afb130, 12) ==
            bytes.fromhex('6890009008610c91080000f9'),
            'Original IOHIDEvent constructor vptr')
    driver = reference.pointers(0xfffffff005d06a68, 220)
    event = reference.pointers(0xfffffff005d07318, 37)
    require(all(driver) and reference.read(0xfffffff005d06a68 + 220 * 8, 24) == bytes(24),
            'Exact original 220-slot event driver')
    require(all(event) and reference.read(0xfffffff005d07318 + 37 * 8, 24) == bytes(24),
            'Exact original 37-slot IOHIDEvent')
    require(driver[198] == 0xfffffff004aface8 and
            event[18] == 0xfffffff004afcf90 and event[23] == 0xfffffff004afcfb8 and
            event[24] == 0xfffffff004afefdc and event[25] == 0xfffffff004afe4c4,
            'Original dispatch and event field virtuals')
    require(reference.read(event[18], 12) == bytes.fromhex('080840f9000540b9c0035fd6'),
            'Original event type accessor layout')
    require(reference.read(0xfffffff004afcf04, 8) == bytes.fromhex('000c40f9a00100b4'),
            'Original appendChild children pointer at 24')
    # Integer accessors used by bounded Consumer event diagnostics. Check the
    # original jump table and loads, not constants copied from a newer SDK.
    require(reference.read(0xfffffff0042e94b6, 2) == bytes.fromhex('1d00') and
            reference.read(0xfffffff0042e95dc, 6) == bytes.fromhex('7e018001c801') and
            reference.read(0xfffffff004afd6b0, 4) == bytes.fromhex('00214079') and
            reference.read(0xfffffff004afd6b8, 4) == bytes.fromhex('00254079') and
            reference.read(0xfffffff004afd7d8, 4) == bytes.fromhex('001540b9'),
            'Original Keyboard type and usagePage/usage/down field dispatch')
    return driver
