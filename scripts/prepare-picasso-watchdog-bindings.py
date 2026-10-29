#!/usr/bin/env python3
"""Verify the original 21H461 IOWatchdog ABI and emit native link aliases.

The input is the decompressed, unmodified, ordinary ARM64 kernelcache used by
NativePrelink. This script never modifies it. A changed image, slide, layout or
function body is rejected before any assembly output is written.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
COMPONENT = 'com.apple.driver.AppleARMWatchdogTimer'
EXPECTED_SLIDE = -0x1000000
BASE_VPTR = 0xfffffff006f247e8
APPLE_VPTR = 0xfffffff006f23a70
IOSERVICE_VTABLE = 0xfffffff0071a1bf0
PURE_VIRTUAL = 0xfffffff0077785e0

# Only recovered, exact original addresses. Local C names explicitly describe
# machine-ABI aliases for functions whose original C++ names are unavailable.
STOCK_BINDINGS = {
    '__ZN10IOWatchdogC2EPK11OSMetaClass': 0xfffffff00663e400,
    '__ZN10IOWatchdogD2Ev': 0xfffffff00663e420,
    '__ZNK10IOWatchdog12getMetaClassEv': 0xfffffff00663e42c,
    '__ZN10IOWatchdog4stopEP9IOService': 0xfffffff00663eb6c,
    '__ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient': 0xfffffff00663ee14,
    '__ZN10IOWatchdog10gMetaClassE': 0xfffffff007b2d268,
    '__ZN10IOWatchdog9metaClassE': 0xfffffff006f247c8,
    '__ZN10IOWatchdog10superClassE': 0xfffffff006f247d0,
    '__ZTV10IOWatchdog': 0xfffffff006f247d8,
    '_picasso_original_iowatchdog_policy_start': 0xfffffff00663e498,
    '_picasso_original_iowatchdog_policy_check': 0xfffffff00663ebd4,
}

# Whole function/body fingerprints supplement the whole-image identity gate.
# The periodic check includes no indirect virtual call or hardware pet.
FINGERPRINTS = {
    'base_C2': (0xfffffff00663e400, 0x20, '60dfcbc3d33e6020176a3041ce012a66987b992e0924005442b6eecf36f2ed95'),
    'base_D2': (0xfffffff00663e420, 4, 'ef07186dae6c85175aa4a3490fa1c9f659b14b1f648b92ab8e52ae2fb7c9414e'),
    'getMetaClass': (0xfffffff00663e42c, 12, '072fa353313526e617e5f3990d317e170567386ea55f5b52efa51f2084388626'),
    'stop': (0xfffffff00663eb6c, 0x40, '92e666b09f07b6bdf36a366487fd473aa9429b0d77e27e406b73245ead7f128e'),
    'newUserClient': (0xfffffff00663ee14, 0x190, '452e46eb53181d443d7f078eaec5bc9fbc996d2bce952d6d00d259cd3bc3e531'),
    'policy_start': (0xfffffff00663e498, 0x3a8, '8a81a4bbe0c4d451a57962b8d40b75ab1c4e049b598f578f89b9f49939bfa9d5'),
    'policy_check': (0xfffffff00663ebd4, 0xd4, 'ab51613166a2182759a0c4281522388ae7714a850af385a0de5654234926fd50'),
    'apfs_event_handler': (0xfffffff00663f4e4, 0x98, '6412266139e8f41bbf84d0eb98cd1459a90fa957750e720f86618249d1ad7896'),
    'base_meta_initializer': (0xfffffff00663f584, 0x3c, '563902bc8b0e5cf46de38a72ffdd81d80cb8bf26dfd231a8646bc5fd719f64f2'),
    'base_vtable': (BASE_VPTR, 171 * 8, 'e584bc603869d2e69290547b2f7c5363d14b3020056a209203ccf5ea93a0fb43'),
}

# AArch64 mov w3,#classSize at the original metaclass constructors.
CLASS_SIZE_SITES = {
    'IOService': (0xfffffff0077c7950, 0x88),
    'IOWatchdog': (0xfffffff00663e3e0, 0xf8),
    'IOWatchdog_initializer': (0xfffffff00663f5a4, 0xf8),
    'IOWatchdogUserClient': (0xfffffff00663e3a0, 0xe8),
    'AppleARMWatchdogTimer': (0xfffffff00663c018, 0x1c8),
}
BASE_OVERRIDES = {
    0: 0xfffffff00663e424,
    1: 0xfffffff00663e428,
    7: 0xfffffff00663e42c,
    87: 0xfffffff00663eb6c,
    140: 0xfffffff00663ee14,
}
APPLE_HARDWARE_SLOTS = [0xfffffff00663dcb4, 0xfffffff00663dd5c, 0xfffffff00663dc7c]
NATIVE_VTABLE_SYMBOL = '__ZTV21PicassoWatchdogDriver'
NATIVE_INITIALIZER = '__GLOBAL__sub_I_PicassoWatchdog.cpp'
NATIVE_OVERRIDES = {
    0: '__ZN21PicassoWatchdogDriverD1Ev',
    1: '__ZN21PicassoWatchdogDriverD0Ev',
    7: '__ZNK21PicassoWatchdogDriver12getMetaClassEv',
    14: '__ZN21PicassoWatchdogDriver4freeEv',
    86: '__ZN21PicassoWatchdogDriver5startEP9IOService',
    111: '__ZNK21PicassoWatchdogDriver11getWorkLoopEv',
    140: '__ZN21PicassoWatchdogDriver13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient',
    168: '__ZN21PicassoWatchdogDriver22enableWatchdogHardwareEv',
    169: '__ZN21PicassoWatchdogDriver23disableWatchdogHardwareEv',
    170: '__ZN21PicassoWatchdogDriver19petWatchdogHardwareEv',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_native_prelink(directory):
    source = directory / 'native_prelink.py'
    require(source.is_file(), f'Actual NativePrelink parser is missing: {source}')
    spec = importlib.util.spec_from_file_location('picasso_watchdog_native_prelink', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate(kernelcache, slide, native_prelink_dir):
    require(slide == EXPECTED_SLIDE,
            f'Unaudited prelink slide {slide:#x}; expected {EXPECTED_SLIDE:#x}')
    data = kernelcache.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    require(digest == ORIGINAL_SHA256,
            f'Original kernelcache SHA256 mismatch: {digest}; expected {ORIGINAL_SHA256}')
    native = load_native_prelink(native_prelink_dir)
    image = native.macho(data)

    def read(address, length):
        offset = native.backed_offset(image['segments'], address, length, len(data))
        return data[offset:offset + length]

    def pointer(address):
        return struct.unpack('<Q', read(address, 8))[0]

    infos = [section for segment in image['segments'] if segment['name'] == '__PRELINK_INFO'
             for section in segment['sections'] if section['name'] == '__info']
    require(len(infos) == 1, 'Expected exactly one original prelink info section')
    metadata = native.unserialize_xml(read(infos[0]['va'], infos[0]['size']))
    entries = metadata['_PrelinkInfoDictionary']
    components = [entry for entry in entries if entry.get('CFBundleIdentifier') == COMPONENT]
    require(len(components) == 1, 'Original AppleARMWatchdogTimer component missing or duplicated')
    component = components[0]
    header = native.backed_offset(image['segments'], component['_PrelinkExecutableLoadAddr'], 32, len(data))
    kext = native.macho(data, header)
    for symbol, address in STOCK_BINDINGS.items():
        owners = [segment for segment in kext['segments']
                  if segment['va'] <= address < segment['va'] + segment['size']]
        require(len(owners) == 1, f'Binding is outside the original watchdog component: {symbol}')
        require(0 <= address + slide < (1 << 64), f'Slid address overflow: {symbol}')

    exports = native.exports(data, image)
    require('__ZTV9IOService' in exports.get(IOSERVICE_VTABLE, []), 'Original IOService vtable moved')
    require('___cxa_pure_virtual' in exports.get(PURE_VIRTUAL, []), 'Original pure virtual target moved')

    class_sizes = {}
    for name, (address, size) in CLASS_SIZE_SITES.items():
        expected = 0x52800003 | (size << 5)  # movz w3,#size
        instruction = struct.unpack('<I', read(address, 4))[0]
        require(instruction == expected, f'{name} class-size instruction changed at {address:#x}')
        class_sizes[name] = {'size': hex(size), 'instruction_address': hex(address),
                             'instruction_bytes': read(address, 4).hex()}

    fingerprints = {}
    for name, (address, length, expected) in FINGERPRINTS.items():
        actual = hashlib.sha256(read(address, length)).hexdigest()
        require(actual == expected, f'Original {name} body changed at {address:#x}')
        fingerprints[name] = {'address': hex(address), 'size': hex(length), 'sha256': actual}

    overrides = {}
    slots = []
    for index in range(171):
        actual = pointer(BASE_VPTR + index * 8)
        if index < 168:
            inherited = pointer(IOSERVICE_VTABLE + 16 + index * 8)
            expected = BASE_OVERRIDES.get(index, inherited)
            if actual != inherited:
                overrides[index] = actual
        else:
            expected = PURE_VIRTUAL
            require(pointer(APPLE_VPTR + index * 8) == APPLE_HARDWARE_SLOTS[index - 168],
                    f'Original Apple hardware slot {index} moved')
        require(actual == expected, f'Original IOWatchdog vtable slot {index} changed')
        slots.append({'index': index, 'offset': hex(index * 8), 'original_target': hex(actual)})
    require(overrides == BASE_OVERRIDES, 'Original IOWatchdog override inventory changed')
    require(pointer(0xfffffff006f247c8) == 0xfffffff007b2d268, 'Original metaClass pointer changed')
    require(pointer(0xfffffff006f247d0) == 0xfffffff007942cb8, 'Original superclass pointer changed')
    require(read(BASE_VPTR - 16, 16) == bytes(16), 'Original vtable header changed')

    # These links must remain intact: removing the Apple kext loses the APFS
    # global collaboration even if a service with a similar name is published.
    apfs = [entry for entry in entries if entry.get('CFBundleIdentifier') == 'com.apple.filesystems.apfs']
    require(len(apfs) == 1 and COMPONENT in apfs[0].get('OSBundleLibraries', {}),
            'Original APFS watchdog dependency changed')
    require(pointer(0xfffffff006f25148) == 0xfffffff00663f4e4, 'APFS watchdog event-handler binding changed')
    require(pointer(0xfffffff006f25150) == 0xfffffff006f247c8, 'APFS watchdog metaclass binding changed')

    return {
        'component': COMPONENT,
        'original_kernelcache': str(kernelcache.resolve()),
        'original_sha256': digest,
        'native_prelink_parser': str((native_prelink_dir / 'native_prelink.py').resolve()),
        'native_prelink_parser_sha256': hashlib.sha256((native_prelink_dir / 'native_prelink.py').read_bytes()).hexdigest(),
        'slide': hex(slide),
        'class_sizes': class_sizes,
        'fingerprints': fingerprints,
        'vtable_address_point': hex(BASE_VPTR),
        'vtable_slots': slots,
        'stock_base': {symbol: hex(address) for symbol, address in STOCK_BINDINGS.items()},
        'candidate_targets': {symbol: hex(address + slide) for symbol, address in STOCK_BINDINGS.items()},
        'policy_start_abi': ['x0=this', 'x1=provider', 'x2=0 (unused)', 'x3=flags bits 0/1', 'w0=bool result'],
        'policy_check_abi': ['x0=this', 'void return; no hardware pet'],
        'apfs_dependency_and_bindings_verified': True,
        'runtime_qualified': False,
    }


def audit_object(path, native_dir, bindings):
    """Audit the actual compiled PicassoWatchdog object, before or after late-init conversion.

    This inspects Mach-O symbols and pointer relocations, not a host mock of
    IOKit. The builder remains responsible for compiling with the real record,
    removing __mod_init_func, globalizing the exact initializer and verifying
    that the conversion leaves executable section bytes unchanged.
    """
    path, native_dir = Path(path), Path(native_dir)
    expected_targets = {name: hex(address + EXPECTED_SLIDE) for name, address in STOCK_BINDINGS.items()}
    require(bindings.get('original_sha256') == ORIGINAL_SHA256 and
            bindings.get('slide') == hex(EXPECTED_SLIDE) and
            bindings.get('candidate_targets') == expected_targets,
            'Object audit received inconsistent original-image bindings')
    original_path = Path(bindings['original_kernelcache'])
    original_data = original_path.read_bytes()
    require(hashlib.sha256(original_data).hexdigest() == ORIGINAL_SHA256,
            'Object audit original kernelcache identity mismatch')
    native = load_native_prelink(native_dir)
    original = native.macho(original_data)
    original_symbols = {name: address for address, names in native.exports(original_data, original).items()
                        for name in names}

    def original_pointer(address):
        offset = native.backed_offset(original['segments'], address, 8, len(original_data))
        return struct.unpack_from('<Q', original_data, offset)[0]

    data = path.read_bytes()
    image = native.macho(data)
    require(image['kind'] == 1, 'Watchdog object is not an ordinary ARM64 Mach-O relocatable')
    build_versions = [struct.unpack_from('<II', data, offset + 8)
                      for command, offset, length in image['commands'] if command == 0x32]
    require(build_versions == [(2, 0x110000)], 'Watchdog object deployment is not iOS 17')
    tables = [offset for command, offset, length in image['commands'] if command == 2 and length == 24]
    require(len(tables) == 1, 'Watchdog object must have one symbol table')
    symbol_offset, count, string_offset, string_size = struct.unpack_from('<4I', data, tables[0] + 8)
    require(symbol_offset + count * 16 <= len(data) and string_offset + string_size <= len(data),
            'Watchdog object symbol table extends past EOF')
    strings = data[string_offset:string_offset + string_size]
    symbols = []
    for index in range(count):
        string_index, kind, section, description, value = struct.unpack_from('<IBBHQ', data, symbol_offset + index * 16)
        end = strings.find(b'\0', string_index)
        require(string_index < len(strings) and end >= 0, 'Unterminated watchdog object symbol')
        symbols.append({'name': strings[string_index:end].decode('utf-8'), 'kind': kind,
                        'section': section, 'value': value})
    defined = [symbol for symbol in symbols if not symbol['kind'] & 0xe0 and symbol['kind'] & 0x0e]
    definitions = {symbol['name']: symbol for symbol in defined}
    require(NATIVE_VTABLE_SYMBOL in definitions, 'Compiled native watchdog vtable is missing')
    require(NATIVE_INITIALIZER in definitions, 'Exact native watchdog static initializer is missing')
    require(not any('10IOWatchdog' in symbol['name'] for symbol in defined),
            'Compiled object defines original IOWatchdog class data/code instead of importing it')

    sections = [section for segment in image['segments'] for section in segment['sections']]
    pointer_relocations = {}
    for section in sections:
        relocation_offset, relocation_count = struct.unpack_from('<II', data, section['command_offset'] + 56)
        require(relocation_offset + relocation_count * 8 <= len(data), 'Relocations extend past EOF')
        for index in range(relocation_count):
            offset, bits = struct.unpack_from('<II', data, relocation_offset + index * 8)
            # Only unsigned, non-PC-relative 64-bit pointer relocations describe
            # vtables/class data. Instructions have separate ARM64 relocations.
            if bits >> 28 != 0 or (bits >> 25) & 3 != 3 or (bits >> 24) & 1:
                continue
            require(offset + 8 <= section['size'], 'Pointer relocation extends past section')
            at = section['va'] + offset
            if (bits >> 27) & 1:
                symbol_index = bits & 0xffffff
                require(symbol_index < len(symbols), 'Invalid pointer relocation symbol index')
                name = symbols[symbol_index]['name']
            else:
                # A compiler-local initializer may use a local section relocation.
                ordinal = bits & 0xffffff
                require(0 < ordinal <= len(sections), 'Invalid local pointer section ordinal')
                target = struct.unpack_from('<Q', data, section['fileoff'] + offset)[0]
                matches = [symbol['name'] for symbol in defined
                           if symbol['section'] == ordinal and symbol['value'] == target]
                name = NATIVE_INITIALIZER if NATIVE_INITIALIZER in matches else None
            require(at not in pointer_relocations, 'Duplicate pointer relocations at one address')
            pointer_relocations[at] = name

    table = definitions[NATIVE_VTABLE_SYMBOL]['value']
    table_sections = [section for section in sections
                      if section['va'] <= table and table + 16 + 171 * 8 <= section['va'] + section['size']]
    require(len(table_sections) == 1, 'Native watchdog vtable is truncated or ambiguous')
    table_section = table_sections[0]
    header_offset = table_section['fileoff'] + table - table_section['va']
    require(data[header_offset:header_offset + 16] == bytes(16), 'Unexpected native vtable header')
    slots = [pointer_relocations.get(table + 16 + index * 8) for index in range(171)]
    require(all(slots), 'Native watchdog vtable has a missing/non-pointer slot')
    require(table + 16 + 171 * 8 not in pointer_relocations, 'Native watchdog added unexpected virtual slots')
    for index, name in enumerate(slots):
        if index in NATIVE_OVERRIDES:
            require(name == NATIVE_OVERRIDES[index], f'Native override slot {index} mismatch: {name}')
        else:
            target = STOCK_BINDINGS.get(name, original_symbols.get(name))
            require(target == original_pointer(BASE_VPTR + index * 8),
                    f'Inherited native slot {index} changed original behavior: {name}')

    superclass_symbol = '__ZN21PicassoWatchdogDriver10superClassE'
    require(superclass_symbol in definitions and
            pointer_relocations.get(definitions[superclass_symbol]['value']) == '__ZN10IOWatchdog10gMetaClassE',
            'Native watchdog superclass is not the original IOWatchdog metaclass')
    undefined = {symbol['name'] for symbol in symbols if symbol['kind'] == 1}
    imports = {name for name in undefined if '10IOWatchdog' in name or name.startswith('_picasso_original_iowatchdog_')}
    require(imports <= set(STOCK_BINDINGS), f'Uncovered original-watchdog imports: {sorted(imports - set(STOCK_BINDINGS))}')
    required = {'__ZN10IOWatchdogC2EPK11OSMetaClass', '__ZN10IOWatchdogD2Ev',
                '__ZN10IOWatchdog10gMetaClassE', '_picasso_original_iowatchdog_policy_start',
                '_picasso_original_iowatchdog_policy_check'}
    require(required <= imports, f'Required original-policy imports missing: {sorted(required - imports)}')
    require('__ZTV10IOWatchdog' in imports or
            '__ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient' in imports,
            'Native client observer does not delegate to the original client implementation')

    automatic_initializers = [section for section in sections if section['name'] == '__mod_init_func']
    require(len(automatic_initializers) <= 1, 'Unexpected multiple native initializer sections')
    if automatic_initializers:
        section = automatic_initializers[0]
        require(section['size'] == 8 and pointer_relocations.get(section['va']) == NATIVE_INITIALIZER,
                'Native object has unexpected automatic static initializers')
    return {
        'object': str(path.resolve()), 'object_sha256': hashlib.sha256(data).hexdigest(),
        'original_sha256': ORIGINAL_SHA256, 'slide': hex(EXPECTED_SLIDE),
        'virtual_slots': 171, 'native_slot_names': slots,
        'native_override_indices': sorted(NATIVE_OVERRIDES),
        'original_imports': {name: expected_targets[name] for name in sorted(imports)},
        'native_superclass_verified': True,
        'initializer_symbol': NATIVE_INITIALIZER,
        'automatic_initializers_removed': not automatic_initializers,
        'initializer_symbol_global': bool(definitions[NATIVE_INITIALIZER]['kind'] & 1),
        'runtime_qualified': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernelcache', required=True, type=Path)
    parser.add_argument('--slide', required=True, type=lambda value: int(value, 0),
                        help='Actual board.toml prelink slide; use --slide=-0x1000000')
    parser.add_argument('--native-prelink-dir', type=Path, default=ROOT / 'kits/NativePrelink',
                        help='Directory containing the actual NativePrelink native_prelink.py')
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--object', type=Path,
                        help='Also audit the actual compiled PicassoWatchdog Mach-O object')
    args = parser.parse_args()
    try:
        report = validate(args.kernelcache, args.slide, args.native_prelink_dir)
        object_report = audit_object(args.object, args.native_prelink_dir, report) if args.object else None
    except (ValueError, OSError, KeyError, TypeError, struct.error) as error:
        raise SystemExit(f'picasso-watchdog ABI verification failed: {error}') from error

    # Write nothing until every identity/layout/function/slide check succeeds.
    args.output_dir.mkdir(parents=True, exist_ok=True)
    assembly = ''.join(f'.globl {symbol}\n.set {symbol}, {target}\n'
                       for symbol, target in sorted(report['candidate_targets'].items()))
    (args.output_dir / 'watchdog-bindings.S').write_text(assembly, encoding='utf-8')
    (args.output_dir / 'watchdog-bindings.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if object_report is not None:
        (args.output_dir / 'watchdog-object-audit.json').write_text(json.dumps(object_report, indent=2) + '\n', encoding='utf-8')
    print('picasso-watchdog: original SHA, class sizes, vtable, function bodies and APFS bindings verified')


if __name__ == '__main__':
    main()
