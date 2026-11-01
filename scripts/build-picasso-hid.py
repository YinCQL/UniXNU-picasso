#!/usr/bin/env python3
"""Qualify HID objects for the existing late display archive and linked kernel."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys

# Artifact checks run from the device checkout, while the ABI parser lives in
# the assembled tree containing this builder.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'kits/NativePrelink'))
from picasso_original_abi import OriginalReference, qualify_hid

OWNED = [0, 1, 7, 14, 86, 87, 111, 171, 173, 180]
MEMBERS = ['NativeHidInput.late.o', 'PicassoHidInput.o', 'hid-original-bindings.o']


def header():
    text = '''// Generated from the pinned original IOHIDDevice; opaque private storage.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
    methods = {171: 'OSString *newTransportString() const',
               173: 'OSString *newProductString() const',
               180: 'IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0',
               181: 'IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits)'}
    for slot in range(168, 207):
        text += ' virtual ' + methods.get(slot, f'__attribute__((deprecated("Unqualified prototype: never call"))) void opaque{slot}()') + ';\n'
    return text + '};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'


def audit_object(platform, native, obj, reference):
    require = platform.require
    methods = qualify_hid(reference)
    raw, sections, symbols = platform.inspect_object(obj, native)
    definitions = {s['name']: s['value'] for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0xe}
    require(not any('11IOHIDDevice' in name for name in definitions), 'Original HID class must remain imported')
    vt = definitions['__ZTV14NativeHidInput']
    relocations = {}
    for sec in sections:
        if sec['va'] <= vt < sec['va'] + sec['size']:
            offset, count = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for index in range(count):
                at, bits = struct.unpack_from('<2I', raw, offset + index * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + at] = symbols[bits & 0xffffff]['name']
    compiled = [relocations.get(vt + 16 + index * 8) for index in range(207)]
    require(all(compiled) and vt + 16 + 207 * 8 not in relocations, 'Exact 207-slot owned HID vtable')
    bindings = {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': 0xfffffff004ad94f8,
                '__ZN11IOHIDDeviceD2Ev': 0xfffffff004ad9518,
                '__ZN11IOHIDDevice10gMetaClassE': 0xfffffff006ae6fa0,
                '__ZTV11IOHIDDevice': 0xfffffff005d045b0}
    owned = []
    for index, name in enumerate(compiled):
        if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
            owned.append(index)
        elif '11IOHIDDevice' in name:
            bindings[name] = methods[index]
        else:
            require(index < 168 and reference.names.get(name) == methods[index], 'Exact inherited HID method ' + str(index))
    require(owned == OWNED, 'Only qualified HID methods overridden')
    undefined = {s['name'] for s in symbols if s['kind'] == 1 and '11IOHIDDevice' in s['name']}
    require(undefined == set(bindings), 'All and only actual HID imports bound')
    return dict(original_sha256=hashlib.sha256(reference.data).hexdigest(), bindings=bindings,
                original_slide=-0x1000000, original_slots=207, instance_bytes=208,
                owned_slots=owned, runtime_qualified=False)


def build(root, platform, native, record, out, board):
    reference = OriginalReference(os.environ['UNIXNU_KERNELCACHE'], board)
    qualify_hid(reference)
    (out / 'OriginalHidDeviceABI.h').write_text(header(), encoding='utf-8')
    sources = [root / 'drivers/NativeHidInput/NativeHidInput.cpp', root / 'kits/picasso/PicassoHidInput.cpp']
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        command = platform.compile_command(record, source, obj, root) + ['-I', str(out), '-I', str(source.parent), '-mgeneral-regs-only']
        platform.run(command, obj.with_suffix('.compile.log'), record['directory'])
        platform.inspect_object(obj, native)
        if source.stem == 'NativeHidInput':
            audit = audit_object(platform, native, obj, reference)
            platform.write_json(out / 'hid-object-audit.json', audit)
            late = out / MEMBERS[0]
            objcopy = shutil.which('llvm-objcopy')
            platform.require(objcopy, 'llvm-objcopy required')
            platform.make_late(obj, late, native, objcopy, '__GLOBAL__sub_I_NativeHidInput.cpp')
            objects.append(late)
        else:
            _, sections, symbols = platform.inspect_object(obj, native)
            platform.require(not any(s['name'] == '__mod_init_func' for s in sections), 'Input probe must have no automatic initializer')
            platform.require(any(s['name'] == '_native_buttons_set_sink' and s['kind'] == 15 for s in symbols),
                             'Input probe must define the optional board-button hook')
            objects.append(obj)
    assembly = out / 'hid-original-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(audit['bindings'].items())), encoding='utf-8')
    bindings = out / MEMBERS[2]
    platform.run([record['arguments'][0], '-target', 'arm64-apple-ios17.0', '-c', assembly, '-o', bindings], out / 'hid-bindings.compile.log')
    _, _, symbols = platform.inspect_object(bindings, native)
    platform.require({s['name']: s['value'] for s in symbols if s['kind'] == 3} == audit['bindings'], 'Exact compiled HID aliases')
    objects.append(bindings)
    sources += [root / 'kits/picasso/PicassoHidPlan.h', root / 'scripts/build-picasso-hid.py', root / 'scripts/picasso_original_abi.py']
    return objects, sources


def verify(platform, native, reference, directory, kernel, plan):
    import json
    require = platform.require
    require((directory / 'OriginalHidDeviceABI.h').read_text(encoding='utf-8') == header(), 'Changed generated HID header')
    audit = audit_object(platform, native, directory / 'NativeHidInput.o', reference)
    require(audit == json.loads((directory / 'hid-object-audit.json').read_text()), 'Stale HID object audit')
    initializer = '__GLOBAL__sub_I_NativeHidInput.cpp'
    require(platform.initializer_state(directory / 'NativeHidInput.o', native, initializer, True) ==
            platform.initializer_state(directory / MEMBERS[0], native, initializer, False), 'HID late conversion changed instructions')
    image = native.macho(kernel)
    names = {n: va for va, group in native.exports(kernel, image).items() for n in group}
    vt = names['__ZTV14NativeHidInput'] + 16
    offset = native.backed_offset(image['segments'], vt, 207 * 8, len(kernel))
    compiled = struct.unpack_from('<207Q', kernel, offset)
    original = qualify_hid(reference)
    local = {va for n, va in names.items() if n.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput'))}
    require([i for i, va in enumerate(compiled) if va in local] == OWNED, 'Wrong linked HID overrides')
    require(all(compiled[i] == original[i] for i in range(207) if i not in OWNED), 'Wrong linked original HID dispatch')
    for name, va in audit['bindings'].items():
        require(names.get(name) == va, 'Wrong linked HID alias ' + name)
        require(any(e['module'] == 'com.apple.iokit.IOHIDFamily' and e['va'] <= va < e['va'] + e['size'] for e in plan['extents']), 'HID alias outside retained original family')
    superclass = native.backed_offset(image['segments'], names['__ZN14NativeHidInput10superClassE'], 8, len(kernel))
    require(struct.unpack_from('<Q', kernel, superclass)[0] == audit['bindings']['__ZN11IOHIDDevice10gMetaClassE'],
            'Wrong linked HID superclass')
    for name in ('_picasso_hid_start_later', '_picasso_hid_frame_presented',
                 '_native_hid_input_requested', '_native_touch_set_sink', '_native_buttons_set_sink',
                 '_native_hid_input_allocate', initializer):
        require(name in names, 'Missing HID lifecycle ' + name)
    return audit
