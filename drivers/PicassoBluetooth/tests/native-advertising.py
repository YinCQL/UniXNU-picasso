#!/usr/bin/env python3
"""Execute the built type-0 ARM64 library and audited bluetoothd advertising code.

Requires Unicorn 2.1.4 and the private 20H392 original (never checked in).
This checks native command selection/callbacks, not RF or device performance.
OS services and HCI submission are intercepted; advertising functions execute
the actual original instructions. The old entry is also run to reproduce 2006.
"""
import argparse
import importlib.util
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP


class MachO:
    def __init__(self, data):
        self.data = data
        self.segments, self.sections, self.symbols, self.imports = [], [], {}, {}
        offset = 32
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            cmd, size = struct.unpack_from('<II', data, offset)
            if cmd == 0x19:
                va, length, fileoff, filesize = struct.unpack_from('<QQQQ', data, offset + 24)
                if data[offset + 8:offset + 24].rstrip(b'\0') != b'__PAGEZERO':
                    self.segments.append((va, length, fileoff, filesize))
                for i in range(struct.unpack_from('<I', data, offset + 64)[0]):
                    s = offset + 72 + 80 * i
                    addr, length = struct.unpack_from('<QQ', data, s + 32)
                    flags, first, stride = struct.unpack_from('<III', data, s + 64)
                    self.sections.append((addr, length, flags & 255, first, stride))
            elif cmd == 2:
                symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, offset + 8)
            elif cmd == 0xb:
                indirectoff, _ = struct.unpack_from('<II', data, offset + 56)
            offset += size
        names = []
        for i in range(nsyms):
            nameoff, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
            end = data.index(0, stroff + nameoff)
            name = data[stroff + nameoff:end].decode()
            names.append(name)
            if kind & 0xe == 0xe:
                self.symbols[name] = value
        for addr, length, kind, first, stride in self.sections:
            if kind not in (6, 7, 8):
                continue
            width = stride if kind == 8 else 8
            for i in range(length // width):
                index = struct.unpack_from('<I', data, indirectoff + (first + i) * 4)[0]
                if index < len(names):
                    self.imports[addr + i * width] = (kind, names[index])

    def load(self, machine):
        for va, length, offset, size in self.segments:
            if length:
                machine.mem_map(va, (length + 4095) & ~4095)
                if size:
                    machine.mem_write(va, self.data[offset:offset + size])


class NativeAdvertising:
    STACK = 0x200000000
    ARENA = 0x300000000
    RETURN = 0x400000000
    MODEL = ARENA
    BROADCASTER = ARENA + 0x2000
    PLATFORM = ARENA + 0x8000
    CONFIG = ARENA + 0xa000

    def __init__(self, binary, library, old_entry=False):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.main, self.lib = MachO(binary), MachO(library)
        self.main.load(self.uc)
        self.lib.load(self.uc)
        for address, size in ((self.STACK, 0x10000), (self.ARENA, 0x20000), (self.RETURN, 0x1000)):
            self.uc.mem_map(address, size)
        self.hooks, self.commands, self.advances, self.deferred = {}, [], [], []
        self.initialized = []
        self.last_command = None
        self.submit_error = 0
        for image in (self.main, self.lib):
            for address, (kind, name) in image.imports.items():
                if kind == 8:
                    self.hooks[address] = lambda name=name: self.import_call(name)
                elif name == '___stack_chk_guard':
                    self.write64(address, self.ARENA + 0x1f000)
        self.write64(self.MODEL, 0x100802480)
        self.write64(self.PLATFORM, self.PLATFORM + 0x100)
        self.write64(self.CONFIG, self.CONFIG + 0x100)
        self.zero_stub, self.one_stub = self.ARENA + 0x1e000, self.ARENA + 0x1e004
        self.hooks[self.zero_stub] = lambda: self.ret(0)
        self.hooks[self.one_stub] = lambda: self.ret(1)
        self.write64(self.PLATFORM + 0x100 + 0x168, self.zero_stub)
        self.write64(self.PLATFORM + 0x100 + 0x138, self.one_stub)
        self.write64(self.CONFIG + 0x100 + 0x290, self.zero_stub)
        for address, result in {
            0x10029aba8: self.MODEL, 0x10029a838: self.PLATFORM, 0x10029aa48: self.CONFIG,
            0x10029c4f8: self.CONFIG, 0x100192fac: 1, 0x100192fd4: 0,
            0x100208fa0: 0, 0x100208fd4: 0, 0x10050a3a4: 0,
            0x10019169c: 0, 0x1001928d4: 0, 0x100192930: 0,
            0x1001d03ac: 0, 0x100192c28: 0, 0x1001c2920: 0,
        }.items():
            self.hooks[address] = lambda result=result: self.ret(result)
        self.hooks[0x10010ca98] = self.submit
        self.hooks[0x1001094ac] = self.advance
        self.hooks[0x1001c2284] = lambda: self.fail('unexpected initialization submission failure')
        self.hooks[0x1001c2a9c] = lambda: self.fail('native assertion')
        self.hooks[0x1001c2ed8] = self.enqueue
        # The native GAP completion reaches the C++ broadcaster's async
        # notification boundary here. Record it instead of running its UI queue.
        self.hooks[0x10050a5a0] = self.initialization_complete
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.write64(0x10085cbc0, 0xffffffffffffffff)  # dispatch_once already ran before probing
        self.write32(self.BROADCASTER + 0x88, 1)     # observed pre-probe mode
        if old_entry:
            self.write32(0x10050a18c, 0xd10183ff)     # previous original prologue, no new trampoline
        self.call(self.lib.symbols['_type0_init'])

    def write32(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value))

    def write64(self, address, value):
        self.uc.mem_write(address, struct.pack('<Q', value))

    def read(self, address, size=4):
        return int.from_bytes(self.uc.mem_read(address, size), 'little')

    def x(self, index):
        return self.uc.reg_read(UC_ARM64_REG_X0 + index)

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM64_REG_X0, value)
        self.uc.reg_write(UC_ARM64_REG_PC, self.uc.reg_read(UC_ARM64_REG_LR))

    def fail(self, reason):
        raise AssertionError(f'{reason}, LR={self.uc.reg_read(UC_ARM64_REG_LR):#x}')

    def code(self, machine, address, size, unused):
        if address in self.hooks:
            self.hooks[address]()

    def import_call(self, name):
        if name in ('_memcpy', '_memmove', '___memmove_chk'):
            self.uc.mem_write(self.x(0), bytes(self.uc.mem_read(self.x(1), self.x(2))))
            self.ret(self.x(0))
        elif name in ('_bzero', '_memset', '___memset_chk'):
            count, byte = (self.x(1), 0) if name == '_bzero' else (self.x(2), self.x(1) & 255)
            self.uc.mem_write(self.x(0), bytes([byte]) * count)
            self.ret(self.x(0))
        elif name == '_memcmp':
            left = bytes(self.uc.mem_read(self.x(0), self.x(2)))
            right = bytes(self.uc.mem_read(self.x(1), self.x(2)))
            self.ret(((left > right) - (left < right)) & 0xffffffffffffffff)
        elif name == '_CCRandomGenerateBytes':
            self.uc.mem_write(self.x(0), b'\x55' * self.x(1))
            self.ret()
        elif name == '__dyld_get_image_header':
            self.ret(0x100000000)
        elif name in ('__dyld_get_image_vmaddr_slide', '_os_log_type_enabled'):
            self.ret(0)
        elif name == '_os_log_create':
            self.ret(1)
        else:
            self.fail(f'unexpected OS import {name}')

    def submit(self):
        opcode = self.read(0x1008448e0 + self.x(0) * 24 + 2, 2)
        self.last_command = (opcode, self.x(1))
        self.commands.append(opcode)
        self.ret(self.submit_error)

    def advance(self):
        self.advances.append(self.x(0))
        self.ret()

    def enqueue(self):
        descriptor = self.x(1)
        length = self.read(descriptor, 2) if descriptor else 0
        payload = bytes(self.uc.mem_read(self.read(descriptor + 8, 8), length)) if length else b''
        self.deferred.append((self.x(0), payload))
        self.ret()

    def initialization_complete(self):
        self.initialized.append((self.x(0), self.x(2)))
        self.ret()

    def dispatch(self):
        while self.deferred:
            callback, payload = self.deferred.pop(0)
            event, data = self.ARENA + 0x1c000, self.ARENA + 0x1c100
            self.uc.mem_write(event, struct.pack('<QQ', len(payload), data))
            if payload:
                self.uc.mem_write(data, payload)
            self.call(callback, event)

    def call(self, address, *args, stack=b''):
        sp = self.STACK + 0xf000
        self.uc.mem_write(sp, b'\0' * 0x100)
        if stack:
            self.uc.mem_write(sp, stack)
        self.uc.reg_write(UC_ARM64_REG_SP, sp)
        self.uc.reg_write(UC_ARM64_REG_LR, self.RETURN)
        for i in range(8):
            self.uc.reg_write(UC_ARM64_REG_X0 + i, args[i] if i < len(args) else 0)
        self.uc.emu_start(address, self.RETURN, count=100000)
        assert self.uc.reg_read(UC_ARM64_REG_PC) == self.RETURN, f'did not return: {address:#x}'
        assert self.uc.reg_read(UC_ARM64_REG_SP) == sp, 'trampoline/native stack imbalance'
        return self.x(0)

    def probe(self, sets=5, status=0, extended=True):
        self.uc.mem_write(0x100863938, b'\xff' * 47)
        self.write64(0x1008638e4, 0x0f01f9ff if extended else 0)
        self.call(self.lib.symbols['_publish'])
        if extended:
            assert self.last_command[0] == 0x203b
            self.call(self.last_command[1], status, sets)

    def start(self):
        self.call(0x10050a18c, self.BROADCASTER)

    def parameters(self, handle=0, data=b'', scan_response=b''):
        # Native LE_StartAdvertising(callback, context, min, max, type, peer,
        # channels, filter, ...), including Darwin's packed stack arguments.
        stack = bytearray(64)
        for length_at, pointer_at, address, payload in (
            (0, 8, self.ARENA + 0x18000, data),
            (18, 24, self.ARENA + 0x18100, scan_response),
        ):
            if payload:
                self.uc.mem_write(address, payload)
                struct.pack_into('<H', stack, length_at, len(payload))
                struct.pack_into('<Q', stack, pointer_at, address)
        stack[33] = handle
        self.call(0x100175da8, self.zero_stub, 0, 48, 48, 0, 0, 7, 0, stack=bytes(stack))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'userland/patch-ios16.py'
    spec = importlib.util.spec_from_file_location('bt_patch', script)
    patcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patcher)
    binary, _ = patcher.patch(args.original.read_bytes())
    library = args.library.read_bytes()

    old = NativeAdvertising(binary, library, old_entry=True)
    old.probe()
    old.start()
    assert old.read(0x100854ab8, 1) == 0
    old.parameters()
    assert old.commands[-1] == 0x2006, old.commands
    print('Previous entry reproduced: 203B followed by legacy 2006; advertising handles remain zero')

    current = NativeAdvertising(binary, library)
    current.probe()
    assert current.read(current.MODEL + 0xa0, 1) == 1
    current.start()
    assert current.read(current.BROADCASTER + 0x88) == 0
    assert current.read(0x100854ab8, 1) == 5
    assert current.commands[-1] == 0x203a, current.commands
    current.call(current.last_command[1], 0, 1650)
    assert current.read(0x100854afa, 2) == 1650
    current.dispatch()
    assert current.initialized == [(0, 5)]
    assert current.read(0x100854af0, 8) == current.read(0x100859bb0, 8) == 0
    current.parameters(data=b'\x02\x01\x06', scan_response=b'\x02\x09A')
    assert current.commands[-1] == 0x2036, current.commands
    current.call(current.last_command[1], 0, 0)
    assert current.commands[-1] == 0x2037, current.commands
    current.call(current.last_command[1], 0)
    assert current.commands[-1] == 0x2038, current.commands
    current.call(current.last_command[1], 0)
    assert current.commands[-1] == 0x2039, current.commands
    current.call(current.last_command[1], 0)
    assert current.read(0x100850918 + 0xd0d, 1) == 2
    assert not set(current.commands) & set(range(0x2006, 0x200b))
    print('New entry: 203B -> native 203A -> 2036 -> 2037 -> 2038 -> 2039, native success')

    first = len(current.commands)
    current.parameters(handle=1, data=b'\x02\x01\x06')
    for opcode in (0x2036, 0x2037, 0x2035, 0x2039):
        assert current.commands[-1] == opcode, current.commands
        current.call(current.last_command[1], 0, 0)
    assert current.commands[first:] == [0x2036, 0x2037, 0x2035, 0x2039]
    assert current.read(0x100850918 + 0xd20 + 0xd0d, 1) == 2
    print('Second native set: 2036 -> 2037 -> 2035 random address -> 2039, separate handle active')

    current.call(0x100177450)
    assert current.commands[-1] == 0x203d
    assert current.read(0x100854ab8, 1) == current.read(0x100854af0, 8) == 0
    current.write32(current.BROADCASTER + 0x88, 1)
    current.start()
    assert current.read(current.BROADCASTER + 0x88) == 0
    assert current.read(0x100854ab8, 1) == 5
    assert current.commands[-1] == 0x203a
    current.call(current.last_command[1], 0, 1650)
    current.dispatch()
    assert current.initialized == [(0, 5), (0, 5)]
    print('Native clear/reopen: 203D clears handles, next start runs 203A and completes again')

    init_error = NativeAdvertising(binary, library)
    init_error.probe()
    init_error.start()
    init_error.call(init_error.last_command[1], 712, 0)
    init_error.dispatch()
    assert init_error.initialized == [(712, 5)]
    assert init_error.read(0x100854af0, 8) == init_error.read(0x100859bb0, 8) == 0
    assert init_error.read(0x100854afa, 2) == 0
    print('Failed 203A: native asynchronous completion preserves status 712 and clears callbacks')

    failed = NativeAdvertising(binary, library)
    failed.probe(status=712)
    assert failed.advances == [712]
    assert failed.read(failed.MODEL + 0xa0, 1) == 0
    print('Failed capacity query: real error propagated, no capability success published')

    for sets in (0, 1, 9):
        limited = NativeAdvertising(binary, library)
        limited.probe(sets=sets)
        assert limited.read(0x100850910, 1) == min(sets, 5)
        limited.call(limited.lib.symbols['_prepare_advertising'], limited.BROADCASTER)
        assert limited.read(limited.BROADCASTER + 0x88) == (0 if sets else 1)
    print('Zero/one/oversized capacity: follows real result and native five-entry capacity')


if __name__ == '__main__':
    main()
