#!/usr/bin/env python3
"""Build the CPU GOP provider against the pinned original iOS17 Legacy ABI.

Uses the original kernelcache directly, never a previous development candidate
or a recorded QEMU result. SDK compilation and final linked dispatch are checked
independently; this script makes no runtime qualification claim.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
SLIDE = -0x1000000
FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'
PURE = [211, 213, 245, 246, 306, 307, 337, 338, 345]
OWNED = sorted([0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301] + PURE)


def require(value, reason):
    if not value:
        raise ValueError(reason)


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(data):
    return hashlib.sha256(data).hexdigest()


def qualify(native, original, slide=SLIDE):
    require(sha(original) == ORIGINAL_SHA and slide == SLIDE, 'Unqualified display image or slide')
    image = native.macho(original)
    symbols = {n: v for v, group in native.exports(original, image).items() for n in group}

    # Existing qualification addresses name the uniformly rebased family. Code
    # is position independent; read unchanged bytes in the pinned raw image.
    def read(address, count):
        at = native.backed_offset(image['segments'], address - slide, count, len(original))
        return original[at:at + count]

    def vptr(pc):
        a, b, c = struct.unpack('<3I', read(pc, 12))
        require(a & 0x9f00001f == 0x90000008 and b & 0xffc003ff == 0x91000108 and c == 0x91004108,
                'Original constructor vptr sequence changed')
        immediate = ((a >> 5) & 0x7ffff) * 4 + ((a >> 29) & 3)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        return (pc & ~4095) + (immediate << 12) + ((b >> 10) & 4095) + 16

    def branch(pc):
        word, = struct.unpack('<I', read(pc, 4))
        require(word & 0xfc000000 == 0x94000000, 'Expected original direct BL')
        immediate = word & 0x3ffffff
        return pc + 4 * (immediate - (1 << 26) if immediate & (1 << 25) else immediate)

    base = struct.unpack('<262Q', read(vptr(0xfffffff00577b324), 262 * 8))
    service = struct.unpack('<262Q', read(vptr(0xfffffff00577b51c), 262 * 8))
    pointer = vptr(0xfffffff0057865d8)
    slots = list(struct.unpack('<358Q', read(pointer, 358 * 8)))
    require(all(slots) and read(pointer + 358 * 8, 24) == bytes(24), 'Wrong original vtable extent')
    require([i for i, v in enumerate(slots) if v == symbols['___cxa_pure_virtual']] == PURE,
            'Original abstract slots changed')
    require([i for i, (a, b) in enumerate(zip(base, service)) if a != b] == [0, 1, 7],
            'Original Service inheritance changed')
    changed = [i for i, (a, b) in enumerate(zip(base, slots)) if a != b]
    require(len(changed) == 103 and [i for i in changed if i < 168] ==
            [0, 1, 7, 14, 21, 44, 79, 85, 86, 87, 111, 141, 155], 'Original Legacy inheritance changed')
    require(base[141] == symbols['__ZN9IOService13newUserClientEP4taskPvjPP12IOUserClient'],
            'Wrong native newUserClient ABI')
    require(branch(0xfffffff0057865fc) == 0xfffffff00577b510 and
            branch(0xfffffff00577b540) == 0xfffffff00577b318 and
            branch(0xfffffff005786630) == 0xfffffff00577b55c, 'Original C2/D2 chain changed')
    checks = {
        0xfffffff005787ef8: '080040f9088144f900013fd6e0000037',
        0xfffffff005795b98: 'e00740f9280080520101001272d3ff97',
        0xfffffff00578aa7c: '080040f9084545f9030080d2e20303aa0400805200013fd6',
        0xfffffff005777e9c: '080040f908e142f900013fd6c0000036',
        0xfffffff005796720: 'ff0302d1fd7b07a9fdc30191a0831ff8',
        0xfffffff005796830: '08ed4af9',
        0xfffffff00579685c: 'e2230091c3008052240080521684ff97',
        0xfffffff005795fbc: '0ab182d2',
        0xfffffff00579ea68: '888888d28888a0f2',
        0xfffffff00579ec54: '08b544f9',
        0xfffffff00579ebb8: 'ff0302d1fd7b07a9fdc30191a0031ff8',
        0xfffffff00579ed54: '086540f90173ff90218c0c9100013fd6',
    }
    for address, expected in checks.items():
        require(read(address, len(bytes.fromhex(expected))).hex() == expected,
                f'Original display contract changed at {address:#x}')
    require(slots[295] + slide == 0xfffffff005795b88 and slots[301] + slide == 0xfffffff00579ea5c,
            'Wrong completion/refresh entries')
    for offset, field in [(0x30, 5808), (0x58, 5812)]:
        word, = struct.unpack('<I', read(slots[249] + slide + offset, 4))
        require(word == 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8, 'Wrong dimensions layout')
    aliases = {
        '__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
        '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
        '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
        '__ZTV25IOMobileFramebufferLegacy': pointer - 16,
        '_native_legacy_vsync_notify': 0xfffffff005796720,
        '_native_legacy_set_refresh_properties': 0xfffffff00579ebb8,
    }
    for name, address, field, wide in [
        ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
        ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
        ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
        ('planes', 0xfffffff004b9ebf4, 152, False),
    ]:
        instruction = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
        require(read(address, 8) == struct.pack('<2I', instruction, 0xd65f03c0), 'Wrong IOSurface getter ' + name)
        aliases['_native_legacy_surface_' + name] = address
    return slots, aliases, symbols


def header(template):
    methods = {
        184: 'virtual bool isMainDisplay();', 188: 'virtual IOReturn getDotPitch(uint32_t *);',
        213: 'virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;',
        249: 'virtual void getDisplayDimensions(uint32_t *,uint32_t *);',
        288: 'virtual bool startHardware();', 290: 'virtual void kickSwapQueue();',
        295: 'virtual void completeCurrentSwap();',
        296: 'virtual bool validateSwap(const OriginalLegacySwapRequest *);',
        301: 'virtual bool getRefreshAttributes(uint64_t *,bool);',
        337: 'virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;',
    }
    for i in range(168, 358):
        template += ' ' + methods.get(i, f'virtual __attribute__((noreturn)) void opaque{i}()=0;' if i in PURE else
            f'virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();') + '\n'
    return template + '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Original instance extent");\n'


def audit_object(platform, native, obj, slots, aliases, original_symbols):
    raw, sections, symbols = platform.inspect_object(obj, native)
    definitions = {s['name']: s['value'] for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0xe}
    require(not any('25IOMobileFramebufferLegacy' in n for n in definitions), 'Original class must remain imported')
    relocations = {}
    for section in sections:
        at, count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
        for i in range(count):
            offset, bits = struct.unpack_from('<2I', raw, at + i * 8)
            if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                relocations[section['va'] + offset] = symbols[bits & 0xffffff]['name']
    table = definitions['__ZTV19NativeLegacyDisplay']
    compiled = [relocations.get(table + 16 + i * 8) for i in range(358)]
    require(all(compiled) and table + 16 + 358 * 8 not in relocations, 'Wrong compiled vtable extent')
    bindings = dict(aliases)
    owned = []
    for i, name in enumerate(compiled):
        if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
            owned.append(i)
        elif '25IOMobileFramebufferLegacy' in name:
            require(i not in PURE, 'Abstract inherited method')
            bindings[name] = slots[i] + SLIDE
        else:
            require(i < 168 and original_symbols.get(name) == slots[i], f'Wrong inherited native slot {i}: {name}')
    require(owned == OWNED, 'Wrong own override set')
    required = {s['name'] for s in symbols if s['kind'] == 1 and '25IOMobileFramebufferLegacy' in s['name']}
    require(required == {n for n in bindings if '25IOMobileFramebufferLegacy' in n}, 'Unqualified original imports')
    require(relocations.get(definitions['__ZN19NativeLegacyDisplay10superClassE']) ==
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE', 'Wrong native superclass')
    return dict(object_sha256=sha(raw), slot_names=compiled, owned_entries=owned, bindings=bindings)


def verify_kernel(aes, native, original, kernel, plan, audit):
    require(sha(original) == ORIGINAL_SHA and plan['original_sha256'] == ORIGINAL_SHA and
            plan['slide'] == SLIDE and plan['candidate_sha256'] == sha(kernel), 'Wrong linked display image')
    require(FAMILY in plan['components'] and 'com.apple.iokit.IOSurface' in plan['components'], 'Missing original display family')
    image = native.macho(kernel)
    symbols = {n: v for v, group in native.exports(kernel, image).items() for n in group}
    methods, local = aes.linked_native_methods(native, kernel, image, {audit['slot_names'][i] for i in OWNED})
    def pointer(address):
        at = native.backed_offset(image['segments'], address, 8, len(kernel))
        return struct.unpack_from('<Q', kernel, at)[0]
    for i, name in enumerate(audit['slot_names']):
        target = methods.get(name) if i in OWNED else symbols.get(name)
        require(target is not None and pointer(symbols['__ZTV19NativeLegacyDisplay'] + 16 + i * 8) == target,
                f'Wrong linked display slot {i}')
    require(pointer(symbols['__ZN19NativeLegacyDisplay10superClassE']) ==
            audit['bindings']['__ZN25IOMobileFramebufferLegacy10gMetaClassE'], 'Wrong linked superclass')
    for name, target in audit['bindings'].items():
        require(symbols.get(name) == target, 'Wrong linked display alias ' + name)
        require(any(e['module'] in (FAMILY, 'com.apple.iokit.IOSurface') and e['va'] <= target < e['va'] + e['size']
                    for e in plan['extents']), 'Display alias outside retained original component ' + name)
    for name in ('_native_legacy_display_probe', '_native_legacy_display_late_probe'):
        require(name in symbols, 'Missing display lifecycle ' + name)
    for name in ('_picasso_keystore_probe', '_picasso_aks_state_enabled', '_picasso_aks_observed',
                 '_native_keyevents_allocate', '__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
                 '__ZN13AppleKeyStore10gMetaClassE', '__ZN23AppleKeyStoreUserClient10gMetaClassE'):
        require(name in symbols, 'Missing state-service lifecycle ' + name)
    require('com.apple.driver.AppleSEPKeyStore' not in plan['components'], 'Conflicting original keystore')
    return dict(kernel_sha256=sha(kernel), virtual_slots=358, owned_entries=OWNED, local_methods=len(local), runtime_qualified=False)


def main():
    platform = load(ROOT / 'scripts/build-picasso-pmgr.py')
    native = load(ROOT / 'kits/NativePrelink/native_prelink.py')
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    board = tomllib.loads((device / 'board.toml').read_text())
    require(board['kernel']['prelink']['kernelcache_sha256'] == ORIGINAL_SHA, 'Wrong configured original')
    require(board['ipsw']['product'] == 'iPad7,1' and board['ipsw']['build'] == '21H461', 'Unqualified state-query ABI')
    require(board['payload']['adt'].get('packed_data_ramdisk') is True and
            not board['payload']['adt'].get('persistent_data') and
            'picasso-aks-state=1' in board['payload']['kernel_args'], 'State service requires explicit RAM Data policy')
    original = Path(os.environ['UNIXNU_KERNELCACHE']).read_bytes()
    slots, aliases, names = qualify(native, original, int(board['kernel']['prelink']['slide'], 0))
    out = ROOT / 'build/native-legacy-display'
    out.mkdir(parents=True, exist_ok=True)
    source_dir = ROOT / 'drivers/NativeMobileDisplay'
    generated = out / 'OriginalLegacyFramebufferABI.h'
    generated.write_text(header((source_dir / 'OriginalLegacyFramebufferBase.h.in').read_text()))
    record = json.loads((ROOT / platform.RECORD).read_text().strip().rstrip(','))
    objects = []
    sources = [source_dir / 'NativeLegacyDisplay.cpp', ROOT / 'kits/picasso/PicassoDisplay.cpp',
               ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp', ROOT / 'kits/picasso/PicassoKeyStore.cpp']
    audit = None
    for source in sources:
        obj = out / (source.stem + '.o')
        command = platform.compile_command(record, source, obj, ROOT) + ['-I', str(out), '-I', str(source_dir), '-mgeneral-regs-only']
        platform.run(command, obj.with_suffix('.compile.log'), record['directory'])
        if source.stem == 'NativeLegacyDisplay':
            audit = audit_object(platform, native, obj, slots, aliases, names)
            platform.write_json(out / 'object-audit.json', audit)
        if source.stem in ('NativeLegacyDisplay', 'NativeKeyStoreEvents'):
            late = out / (source.stem + '.late.o')
            objcopy = shutil.which('llvm-objcopy')
            require(objcopy, 'llvm-objcopy required')
            platform.make_late(obj, late, native, objcopy, '__GLOBAL__sub_I_' + source.name)
            objects.append(late)
        else:
            objects.append(obj)
    assembly = out / 'original-bindings.S'
    assembly.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n, v in sorted(audit['bindings'].items())))
    bindings = out / 'original-bindings.o'
    platform.run([record['arguments'][0], '-target', 'arm64-apple-ios17.0', '-c', assembly, '-o', bindings],
                 out / 'bindings.compile.log')
    _, _, syms = platform.inspect_object(bindings, native)
    require({s['name']: s['value'] for s in syms if s['kind'] == 3} == audit['bindings'], 'Wrong compiled display aliases')
    objects.append(bindings)
    pending = Path(tempfile.mkdtemp(prefix='qualified-display-', dir=out)) / 'libNativeLegacyDisplay.late.a'
    platform.run(['xcrun', 'ar', 'rcs', pending, *objects], out / 'archive.log')
    members = platform.check_archive_members(platform.run(['xcrun', 'ar', 't', pending], out / 'members.log').splitlines(), objects)
    archive = out / pending.name
    platform.save_old_archive(archive)
    pending.rename(archive)
    sources += [source_dir / n for n in ('PicassoGop.h', 'NativeLegacyRequest.h', 'OriginalLegacyFramebufferBase.h.in')]
    sources += [ROOT / 'scripts/build-picasso-display.py', ROOT / 'scripts/build-picasso-pmgr.py']
    platform.write_json(out / 'build-report.json', dict(complete=True, archive_sha256=sha(archive.read_bytes()),
        source_sha256={str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in sources},
        generated_abi_sha256=sha(generated.read_bytes()), original_sha256=sha(original),
        object_audit_sha256=sha((out / 'object-audit.json').read_bytes()), **members, runtime_qualified=False))
    print('Picasso CPU display: SDK objects, original 358-slot ABI and late archive verified')


if __name__ == '__main__':
    main()
