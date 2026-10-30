#!/usr/bin/env python3
"""Build platform services with the recorded XNU ABI and checked late init.

Runs inside the assembled fixed-pin tree. Original-image aliases are generated
only after identity/ABI checks. Old archives and intermediate objects are
preserved; only qualified objects enter the force-loaded platform archive.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
SOURCES = ('PicassoPmgr', 'PicassoWatchdog', 'PicassoBootState')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def run(command, log, cwd=None):
    command = [str(value) for value in command]
    write_json(log.with_suffix('.command.json'), command)
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout, encoding='utf-8')
    require(result.returncode == 0,
            f'Command failed ({result.returncode}); see {log}\n{result.stdout[-3500:]}')
    return result.stdout


def compile_command(record, source, obj, root):
    command, index, inputs, outputs = [], 0, 0, 0
    arguments = record['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            inputs += 1
        elif value == '-o':
            require(index + 1 < len(arguments), 'Missing recorded output argument')
            command.extend(['-o', str(obj)])
            outputs += 1
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ', '--serialize-diagnostics'):
            require(index + 1 < len(arguments), 'Incomplete dependency/diagnostic argument')
            index += 1  # Never overwrite the original compile record/dependencies.
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    require(inputs == outputs == 1, 'Expected one source and output in actual IOPlatformExpert record')
    command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-c99-designator', '-Wno-reorder-init-list',
                    '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
                    '-Wno-unknown-warning-option', '-Wno-missing-designated-field-initializers',
                    '-I', str(root / '.cache/xnu-work/osfmk'), '-I', str(source.parent)])
    return command


def inspect_object(path, native):
    data = path.read_bytes()
    require(len(data) >= 32 and struct.unpack_from('<4I', data) == (0xfeedfacf, 0x100000c, 0, 1),
            f'Expected ordinary ARM64 Mach-O relocatable: {path}')
    image = native.macho(data)
    versions = [struct.unpack_from('<II', data, at + 8)
                for cmd, at, _ in image['commands'] if cmd == 0x32]
    require(versions == [(2, 0x110000)], f'Expected iOS 17 deployment: {path}')
    tables = [at for cmd, at, size in image['commands'] if cmd == 2 and size == 24]
    require(len(tables) == 1, f'Expected one symbol table: {path}')
    at, count, strings_at, strings_size = struct.unpack_from('<4I', data, tables[0] + 8)
    require(at + count * 16 <= len(data) and strings_at + strings_size <= len(data), 'Invalid symbol table')
    strings = data[strings_at:strings_at + strings_size]
    symbols = []
    for index in range(count):
        ix, kind, section, _, value = struct.unpack_from('<IBBHQ', data, at + index * 16)
        end = strings.find(b'\0', ix)
        require(ix < len(strings) and end >= 0, 'Invalid symbol string')
        symbols.append(dict(name=strings[ix:end].decode(), kind=kind, section=section, value=value))
    sections = [s for segment in image['segments'] for s in segment['sections']]
    return data, sections, symbols


def initializer_state(path, native, initializer, automatic):
    data, sections, symbols = inspect_object(path, native)
    definitions = [s for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0x0e == 0xe]
    matches = [s for s in definitions if s['name'] == initializer]
    require(len(matches) == 1, f'Exact compiler initializer missing/duplicated: {initializer}')
    symbol = matches[0]
    require(0 < symbol['section'] <= len(sections) and
            sections[symbol['section'] - 1]['name'] == '__text', 'Initializer must be executable code')
    initializers = [s for s in sections if s['name'] == '__mod_init_func']
    if automatic:
        require(len(initializers) == 1, 'Expected exactly one automatic initializer section')
        section = initializers[0]
        require(section['size'] == 8, 'Unexpected additional static initializer')
        at, count = struct.unpack_from('<II', data, section['command_offset'] + 56)
        require(count == 1 and at + 8 <= len(data), 'Expected one initializer pointer relocation')
        offset, bits = struct.unpack_from('<II', data, at)
        require(offset == 0 and bits >> 28 == 0 and (bits >> 25) & 3 == 3 and not bits & (1 << 24),
                'Unexpected initializer relocation type')
        if bits & (1 << 27):
            ix = bits & 0xffffff
            require(ix < len(symbols) and symbols[ix]['name'] == initializer, 'Wrong automatic initializer target')
        else:
            require(bits & 0xffffff == symbol['section'] and
                    struct.unpack_from('<Q', data, section['fileoff'])[0] == symbol['value'],
                    'Wrong local automatic initializer target')
    else:
        require(not initializers, 'Automatic initializer survived late conversion')
        require(symbol['kind'] & 1, 'Late initializer is not externally linkable')
    # Include all executable sections, not just the expected __text section.
    executable = {}
    for section in sections:
        flags = struct.unpack_from('<I', data, section['command_offset'] + 64)[0]
        if flags & 0x80000400:
            require(section['fileoff'] + section['size'] <= len(data), 'Executable section exceeds EOF')
            key = (section.get('segname', ''), section['name'])
            require(key not in executable, 'Duplicate executable section')
            executable[key] = data[section['fileoff']:section['fileoff'] + section['size']]
    require(executable, 'No executable code in native service')
    return executable


def globalize_initializer(path, native, initializer):
    """Export one local Mach-O initializer, preserving nlist group/index rules.

    LLVM 18 objcopy rejects --globalize-symbol for Mach-O. Merely setting N_EXT
    leaves LC_DYSYMTAB's local/external groups inconsistent. Reorder the nlists
    and remap every symbol-index consumer in an ordinary relocatable instead.
    Unsupported auxiliary tables fail closed; no executable bytes are edited.
    """
    raw, sections, symbols = inspect_object(path, native)
    data = bytearray(raw)
    commands = native.macho(raw)['commands']
    sym_at = next(at for cmd, at, _ in commands if cmd == 2)
    symoff, count, _, _ = struct.unpack_from('<4I', raw, sym_at + 8)
    dynamics = [at for cmd, at, length in commands if cmd == 0xb and length == 80]
    require(len(dynamics) == 1, 'Expected one LC_DYSYMTAB for initializer export')
    at = dynamics[0]
    fields = list(struct.unpack_from('<18I', raw, at + 8))
    local, nlocal, external, nexternal, undefined, nundefined = fields[:6]
    require(local == 0 and external == nlocal and undefined == nlocal + nexternal and
            undefined + nundefined == count, 'Unexpected Mach-O symbol group layout')
    require(all(fields[i] == 0 for i in (7, 9, 11, 15, 17)),
            'Auxiliary module/reference/relocation tables are not supported in service objects')
    matches = [i for i, s in enumerate(symbols) if s['name'] == initializer]
    require(len(matches) == 1, 'Initializer definition is not unique')
    old = matches[0]
    require(old < nlocal and symbols[old]['kind'] == 0xe, 'Initializer is not a local N_SECT symbol')
    order = [i for i in range(undefined) if i != old] + [old] + list(range(undefined, count))
    remap = {previous: current for current, previous in enumerate(order)}
    for current, previous in enumerate(order):
        entry = bytearray(raw[symoff + previous * 16:symoff + (previous + 1) * 16])
        if previous == old:
            entry[4] |= 1  # N_EXT
        data[symoff + current * 16:symoff + (current + 1) * 16] = entry
    for section in sections:
        rel_at, rel_count = struct.unpack_from('<II', raw, section['command_offset'] + 56)
        require(rel_at + rel_count * 8 <= len(raw), 'Invalid section relocations')
        for index in range(rel_count):
            offset, bits = struct.unpack_from('<II', raw, rel_at + index * 8)
            require(not offset & 0x80000000, 'Scattered relocations are not supported on ARM64')
            if bits & (1 << 27):
                previous = bits & 0xffffff
                require(previous < count and bits >> 28 != 10, 'Invalid external symbol relocation')
                struct.pack_into('<I', data, rel_at + index * 8 + 4,
                                 (bits & 0xff000000) | remap[previous])
    indirect_at, indirect_count = fields[12:14]
    require(indirect_at + indirect_count * 4 <= len(raw), 'Invalid indirect symbol table')
    for index in range(indirect_count):
        previous = struct.unpack_from('<I', raw, indirect_at + index * 4)[0]
        if not previous & 0xc0000000:
            require(previous < count, 'Invalid indirect symbol index')
            struct.pack_into('<I', data, indirect_at + index * 4, remap[previous])
    fields[:6] = [0, nlocal - 1, nlocal - 1, nexternal + 1, undefined, nundefined]
    struct.pack_into('<18I', data, at + 8, *fields)
    path.write_bytes(data)


def make_late(obj, late, native, objcopy, initializer):
    before = initializer_state(obj, native, initializer, True)
    run([objcopy, '--remove-section=__DATA,__mod_init_func', obj, late], late.with_suffix('.objcopy.log'))
    globalize_initializer(late, native, initializer)
    after = initializer_state(late, native, initializer, False)
    require(before == after, 'Late registration changed executable section bytes')
    return {'raw_sha256': digest(obj), 'late_sha256': digest(late),
            'initializer': initializer, 'automatic_initializers_removed': True,
            'initializer_symbol_global': True, 'executable_bytes_unchanged': True}


def save_old_archive(archive):
    if archive.exists():
        backup = Path(tempfile.mkdtemp(prefix='previous-platform-', dir=archive.parent))
        archive.rename(backup / archive.name)
        return str(backup / archive.name)
    return None


def check_archive_members(listed, objects):
    # Apple ar lists its generated ranlib index; llvm-ar t hides that entry.
    # Permit one exact Darwin index name at the front, never arbitrary extras.
    darwin_indexes = {'__.SYMDEF', '__.SYMDEF SORTED',
                     '__.SYMDEF_64', '__.SYMDEF_64 SORTED'}
    indexes = listed[:1] if listed and listed[0] in darwin_indexes else []
    members = listed[len(indexes):]
    expected = [obj.name for obj in objects]
    require(members == expected,
            f'Unexpected/stale member in platform archive: expected {expected}, listed {listed}')
    return {'members': members, 'symbol_table_members': indexes, 'listed_members': listed}


def main():
    out = ROOT / 'build/sheng-platform-power'
    out.mkdir(parents=True, exist_ok=True)
    report = {'runtime_qualified': False, 'complete': False, 'objects': {}}
    try:
        device = Path(os.environ['UNIXNU_DEVICE']).resolve()
        board = tomllib.loads((device / 'board.toml').read_text(encoding='utf-8'))
        prelink = board['kernel']['prelink']
        kernelcache = Path(os.environ.get('UNIXNU_KERNELCACHE',
                           str(device / 'out/ipsw' / board['ipsw']['build'] / 'kernelcache.decompressed'))).resolve()
        binding_script = ROOT / 'scripts/prepare-picasso-watchdog-bindings.py'
        spec = importlib.util.spec_from_file_location('picasso_watchdog_bindings', binding_script)
        binding = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(binding)
        require(prelink['kernelcache_sha256'] == binding.ORIGINAL_SHA256,
                'Board kernelcache identity differs from the audited watchdog original')
        native_dir = ROOT / 'kits/NativePrelink'
        bindings = binding.validate(kernelcache, int(prelink['slide'], 0), native_dir)
        native = binding.load_native_prelink(native_dir)
        write_json(out / 'watchdog-bindings.json', bindings)
        assembly = out / 'watchdog-bindings.S'
        assembly.write_text(''.join(f'.globl {name}\n.set {name}, {target}\n'
                           for name, target in sorted(bindings['candidate_targets'].items())), encoding='utf-8')
        record_path = ROOT / RECORD
        record = json.loads(record_path.read_text().strip().rstrip(','))
        objcopy = shutil.which('llvm-objcopy')
        require(objcopy, 'llvm-objcopy is required for explicit late class registration')
        report.update(compile_record_sha256=digest(record_path), board_sha256=digest(device / 'board.toml'),
                      original_sha256=bindings['original_sha256'], slide=bindings['slide'])
        objects = []
        for name in SOURCES:
            source = ROOT / 'kits/picasso' / (name + '.cpp')
            obj = out / (name + '.o')
            run(compile_command(record, source, obj, ROOT), out / (name + '.compile.log'), record['directory'])
            inspect_object(obj, native)
            entry = {'source_sha256': digest(source), 'object_sha256': digest(obj)}
            report['objects'][name] = entry
            if name == 'PicassoWatchdog':
                write_json(out / 'watchdog-object-raw.json', binding.audit_object(obj, native_dir, bindings))
            if name != 'PicassoPmgr':
                late = out / (name + '.late.o')
                entry.update(make_late(obj, late, native, objcopy, '__GLOBAL__sub_I_' + name + '.cpp'))
                obj = late
            if name == 'PicassoWatchdog':
                write_json(out / 'watchdog-object-audit.json', binding.audit_object(obj, native_dir, bindings))
            objects.append(obj)
        aliases = out / 'watchdog-bindings.o'
        run([record['arguments'][0], '-target', 'arm64-apple-ios17.0', '-c', assembly, '-o', aliases],
            out / 'watchdog-bindings.compile.log')
        _, _, symbols = inspect_object(aliases, native)
        actual = {s['name']: hex(s['value']) for s in symbols if s['kind'] == 3}
        require(actual == bindings['candidate_targets'], 'Compiled original-image aliases differ from audit')
        objects.append(aliases)
        # Archive from a new file, then replace the old archive by moving it.
        pending_dir = Path(tempfile.mkdtemp(prefix='qualified-platform-', dir=out))
        pending = pending_dir / 'libShengPlatformPower.a'
        run(['xcrun', 'ar', 'rcs', pending, *objects], out / 'archive.log')
        listed = run(['xcrun', 'ar', 't', pending], out / 'archive-members.log').splitlines()
        report.update(check_archive_members(listed, objects))
        archive = out / pending.name
        report['previous_archive'] = save_old_archive(archive)
        pending.rename(archive)
        report.update(archive_sha256=digest(archive), complete=True)
        print('picasso platform: -Werror ARM64/iOS17 build, original ABI, late init and archive checks passed')
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        write_json(out / 'build-report.json', report)


if __name__ == '__main__':
    main()
