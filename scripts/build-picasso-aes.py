#!/usr/bin/env python3
"""Build software AES against the exact retained IOCryptoAcceleratorFamily ABI."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
SLIDE = -0x1000000
BASE_VPTR = 0xfffffff006cecb90
BASE_META = 0xfffffff007ae52d0
BINDINGS_SHA = '69576361ea9a462cfed936e36e5c08458bb8b710ed1ee523e96f7ec7cf0a960a'
OVERRIDES = {0, 1, 7, 86, 111, 117, 168, 169, 170, 171, 172}


def require(value, message):
    if not value:
        raise ValueError(message)


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(data):
    return hashlib.sha256(data).hexdigest()


def names(native, data, image):
    return {name: address for address, group in native.exports(data, image).items() for name in group}


def read(native, data, image, address, size):
    offset = native.backed_offset(image['segments'], address, size, len(data))
    return data[offset:offset + size]


def qualify_original(native, original, recorded, slide):
    require(sha(original) == ORIGINAL_SHA and slide == SLIDE, 'Unaudited AES image or slide')
    require(sha(recorded) == BINDINGS_SHA, 'Changed pinned AES aliases')
    image = native.macho(original)
    symbols = names(native, original, image)
    aliases = {name: int(address, 16) for name, address in
               re.findall(r'^\.set (\S+), (0x[0-9a-f]+)$', recorded.decode(), re.M)}
    require(len(aliases) == 22, 'Incomplete AES alias table')
    require(aliases['__ZN16IOAESAccelerator10gMetaClassE'] == BASE_META + SLIDE,
            'Wrong AES metaclass')
    slots = list(struct.unpack('<181Q', read(native, original, image, BASE_VPTR, 181 * 8)))
    require(read(native, original, image, BASE_VPTR - 16, 16) == bytes(16), 'Invalid AES vtable header')
    expected = []
    for index, address in enumerate(slots):
        candidates = sorted(name for name, target in aliases.items() if target - SLIDE == address)
        if not candidates:
            candidates = sorted(name for name, target in symbols.items() if target == address)
        require(bool(candidates), f'Unresolved original AES slot {index}')
        expected.append(candidates)
    # Protected constructor installs this exact vptr; original getMetaClass returns this metaclass.
    require(read(native, original, image, 0xfffffff005a80038, 32).hex() ==
            'fd7bbfa9fd0300916c0900946893009008412e91080000f9fd7bc1a8c0035fd6',
            'Original AES constructor changed')
    return aliases, slots, expected


def audit_object(platform, native, path, original, aliases, slots):
    data, sections, symbols = platform.inspect_object(path, native)
    definitions = {s['name']: s for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0x0e}
    original_symbols = names(native, original, native.macho(original))
    require('__ZTV9NativeAES' in definitions, 'Missing real NativeAES vtable')
    require(not any('16IOAESAccelerator' in name or '19IOCryptoAccelerator' in name for name in definitions),
            'Native AES must import the genuine original classes')
    relocations = {}
    for section in sections:
        at, count = struct.unpack_from('<II', data, section['command_offset'] + 56)
        require(at + count * 8 <= len(data), 'Invalid AES relocation extent')
        for index in range(count):
            offset, word = struct.unpack_from('<II', data, at + index * 8)
            if word >> 28 == 0 and word >> 25 & 3 == 3 and not word >> 24 & 1 and word >> 27 & 1:
                require(offset + 8 <= section['size'] and (word & 0xffffff) < len(symbols), 'Invalid AES pointer relocation')
                relocations[section['va'] + offset] = symbols[word & 0xffffff]['name']
    table = definitions['__ZTV9NativeAES']['value']
    slot_names = []
    for index, original_target in enumerate(slots):
        name = relocations.get(table + 16 + index * 8)
        require(name is not None, f'Missing AES slot {index}')
        if index in OVERRIDES:
            parent = name.replace('9NativeAES', '16IOAESAccelerator')
            require('9NativeAES' in name and name in definitions and aliases.get(parent) == original_target + SLIDE,
                    f'Wrong native AES override {index}: {name}')
        else:
            target = aliases[name] - SLIDE if name in aliases else original_symbols.get(name)
            require(target == original_target, f'Changed inherited AES slot {index}: {name}')
        slot_names.append(name)
    require(table + 16 + 181 * 8 not in relocations, 'Unexpected extra AES virtual method')
    superclass = definitions.get('__ZN9NativeAES10superClassE')
    require(superclass and relocations.get(superclass['value']) == '__ZN16IOAESAccelerator10gMetaClassE',
            'Wrong native AES superclass')
    platform.initializer_state(path, native, '__GLOBAL__sub_I_NativeAES.cpp', True)
    return {'object_sha256': sha(data), 'virtual_slots': 181, 'slot_names': slot_names,
            'native_overrides': sorted(OVERRIDES), 'original_superclass_verified': True}


def linked_native_methods(native, data, image, wanted):
    """Resolve exact method definitions, including inline methods made local by ld.

    native.exports intentionally excludes local symbols for prelink rebinding.
    A native vtable can still point at them; require unique, backed executable
    N_SECT definitions instead of changing the upstream export semantics.
    """
    tables = [at for cmd, at, size in image['commands'] if cmd == 2 and size == 24]
    require(len(tables) == 1, 'Expected one linked AES symbol table')
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, tables[0] + 8)
    require(symoff + count * 16 <= len(data) and stroff + strsize <= len(data),
            'Invalid linked AES symbol table')
    strings = data[stroff:stroff + strsize]
    sections = [section for segment in image['segments'] for section in segment['sections']]
    result, local = {}, set()
    for index in range(count):
        strx, kind, section_index, _, address = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if kind & 0xe0:  # Debug entries are not definitions.
            continue
        end = strings.find(b'\0', strx)
        require(strx < strsize and end >= 0, 'Invalid linked AES symbol name')
        name = strings[strx:end].decode()
        if name not in wanted:
            continue
        require(name not in result, 'Ambiguous linked AES method: ' + name)
        require(kind & 0x0e == 0x0e and 0 < section_index <= len(sections),
                'Expected linked AES section definition: ' + name)
        section = sections[section_index - 1]
        flags = struct.unpack_from('<I', data, section['command_offset'] + 64)[0]
        require(flags & 0x80000400 and address % 4 == 0 and
                section['va'] <= address < address + 4 <= section['va'] + section['size'],
                'Linked AES method is not executable: ' + name)
        read(native, data, image, address, 4)
        result[name] = address
        if not kind & 1:
            local.add(name)
    require(set(result) == wanted, 'Missing linked AES methods: ' + ', '.join(sorted(wanted - result.keys())))
    return result, local


def verify_kernel(native, original, kernel, plan, audit, aliases, slots):
    require(sha(original) == ORIGINAL_SHA, 'Wrong original AES kernelcache')
    require(plan['original_sha256'] == ORIGINAL_SHA and plan['slide'] == SLIDE and
            plan['candidate_sha256'] == sha(kernel), 'Wrong AES prelink identity')
    require('com.apple.iokit.IOCryptoAcceleratorFamily' in plan['components'], 'Missing crypto class provider')
    image = native.macho(kernel)
    symbols = names(native, kernel, image)
    require('__ZTV9NativeAES' in symbols, 'Missing linked AES subclass')
    require(len(audit['slot_names']) == 181, 'Incomplete AES object audit')
    methods, local = linked_native_methods(native, kernel, image,
        {audit['slot_names'][index] for index in OVERRIDES})
    pointer = lambda address: struct.unpack('<Q', read(native, kernel, image, address, 8))[0]
    require(pointer(symbols['__ZN9NativeAES10superClassE']) == BASE_META + SLIDE, 'Wrong linked AES superclass')
    for index, name in enumerate(audit['slot_names']):
        target = methods.get(name) if index in OVERRIDES else symbols.get(name)
        require(target is not None and pointer(symbols['__ZTV9NativeAES'] + 16 + index * 8) == target,
                f'Wrong linked AES dispatch slot {index}')
        if name in aliases:
            require(symbols[name] == aliases[name], f'Wrong linked original AES alias {name}')
    require(b"'IOClass'         = NativeAES;" in kernel, 'NativeAES personality is disabled')
    require(b'NativeAESSoftwareKeysOnly\0' in kernel, 'Missing software-key capability marker')
    return {'kernel_sha256': sha(kernel), 'virtual_slots': 181, 'actual_native_superclass_verified': True,
            'local_native_dispatch_slots': [i for i, name in enumerate(audit['slot_names']) if name in local],
            'software_keys_only': True, 'runtime_qualified': False}


def main():
    platform = load(ROOT / 'scripts/build-picasso-pmgr.py')
    native = load(ROOT / 'kits/NativePrelink/native_prelink.py')
    device = Path(os.environ['UNIXNU_DEVICE'])
    board = tomllib.loads((device / 'board.toml').read_text(encoding='utf-8'))
    original = Path(os.environ['UNIXNU_KERNELCACHE']).read_bytes()
    recorded = (ROOT / 'scripts/data/native-aes-original-bindings.S').read_bytes()
    require(board['kernel']['prelink']['kernelcache_sha256'] == ORIGINAL_SHA, 'Wrong board AES identity')
    aliases, slots, _ = qualify_original(native, original, recorded, int(board['kernel']['prelink']['slide'], 0))
    out = ROOT / 'build/native-aes'
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'drivers/NativeAES/NativeAES.cpp'
    obj = out / 'NativeAES.o'
    record = json.loads((ROOT / platform.RECORD).read_text(encoding='utf-8').strip().rstrip(','))
    platform.run(platform.compile_command(record, source, obj, ROOT), out / 'compile.log', record['directory'])
    audit = audit_object(platform, native, obj, original, aliases, slots)
    platform.write_json(out / 'object-audit.json', audit)
    assembly = out / 'original-bindings.S'
    assembly.write_bytes(recorded)
    binding_obj = out / 'original-bindings.o'
    platform.run([record['arguments'][0], '-target', 'arm64-apple-ios17.0', '-c', assembly, '-o', binding_obj], out / 'bindings.log')
    _, _, alias_symbols = platform.inspect_object(binding_obj, native)
    require({s['name']: s['value'] for s in alias_symbols if s['kind'] == 3} == aliases, 'Assembled AES aliases changed')
    pending = Path(tempfile.mkdtemp(prefix='qualified-aes-', dir=out)) / 'libNativeAES.a'
    objects = [obj, binding_obj]
    platform.run(['xcrun', 'ar', 'rcs', pending, *objects], out / 'archive.log')
    members = platform.check_archive_members(platform.run(['xcrun', 'ar', 't', pending], out / 'members.log').splitlines(), objects)
    archive = out / pending.name
    previous = platform.save_old_archive(archive)
    pending.rename(archive)
    platform.write_json(out / 'build-report.json', dict(complete=True, original_sha256=ORIGINAL_SHA,
        slide=SLIDE, source_sha256=sha(source.read_bytes()),
        header_sha256=sha(source.with_name('OriginalAESABI.h').read_bytes()),
        object_sha256=sha(obj.read_bytes()), bindings_sha256=sha(binding_obj.read_bytes()),
        archive_sha256=sha(archive.read_bytes()), previous_archive=previous, runtime_qualified=False, **members))
    print('Picasso AES: real ARM64/iOS17 software AES, 181 original ABI slots and archive qualified')


if __name__ == '__main__':
    main()
