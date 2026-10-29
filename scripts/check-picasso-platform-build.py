#!/usr/bin/env python3
"""Exercise the platform builder with real LLVM Mach-O schema fixtures.

Includes complete archive assembly, late-init conversion, retained old archives,
original-image checks and rejection cases. The schema objects are deliberately
not an SDK compilation of the platform drivers.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--native-prelink-dir', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    builder = load(ROOT / 'scripts/build-picasso-pmgr.py', 'platform_builder')
    binding = load(ROOT / 'scripts/prepare-picasso-watchdog-bindings.py', 'watchdog_binding')
    native = binding.load_native_prelink(args.native_prelink_dir)
    llvm = args.llvm_bin.resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    clang, objcopy, ar = [str(llvm / (name + suffix)) for name in ('clang', 'llvm-objcopy', 'llvm-ar')]
    report = {'scope': 'Real LLVM Mach-O schema objects and complete builder; not production SDK compilation.',
              'builder_sha256': builder.digest(ROOT / 'scripts/build-picasso-pmgr.py'),
              'binding_script_sha256': builder.digest(ROOT / 'scripts/prepare-picasso-watchdog-bindings.py'),
              'checks': []}
    def passed(name):
        report['checks'].append(name)

    def reject(name, operation):
        try:
            operation()
        except (ValueError, RuntimeError):
            passed(name)
        else:
            raise AssertionError('Invalid case was accepted: ' + name)

    def assemble(source, obj):
        subprocess.run([clang, '-target', 'arm64-apple-ios17.0', '-x', 'assembler', '-c',
                        str(source), '-o', str(obj)], check=True, capture_output=True)

    try:
        bindings = binding.validate(args.kernelcache.resolve(), -0x1000000, args.native_prelink_dir)
        passed('original identity, class/vtable/function/APFS ABI')
        reject('wrong slide', lambda: binding.validate(args.kernelcache, 0, args.native_prelink_dir))
        corrupted = out / 'corrupt.macho'
        corrupted.write_bytes(b'not the original')
        reject('wrong original identity', lambda: binding.validate(corrupted, -0x1000000, args.native_prelink_dir))

        # Exercise the real compiler-generated initializer shape, including the
        # explicit asm label used by production. No IOKit declarations are mocked.
        cpp = out / 'PicassoBootState.cpp'
        cpp.write_text('extern void register_class();\n'
            'class Meta { public: Meta() { register_class(); } }; Meta meta;\n'
            'extern "C" void explicit_init() __asm__("__GLOBAL__sub_I_PicassoBootState.cpp");\n'
            'extern "C" void invoke_late() { explicit_init(); }\n')
        obj, late = out / 'cpp.o', out / 'cpp.late.o'
        subprocess.run([clang, '-target', 'arm64-apple-ios17.0', '-std=gnu++2b', '-fapple-kext',
                        '-fsized-deallocation', '-fno-exceptions', '-fno-rtti', '-c', str(cpp), '-o', str(obj)],
                       check=True, capture_output=True)
        builder.make_late(obj, late, native, objcopy, '__GLOBAL__sub_I_PicassoBootState.cpp')
        passed('real C++ static initializer label, globalization, removal and unchanged text')
        caller = out / 'caller.cpp'
        caller.write_text('void register_class() {}\n'
            'extern "C" void explicit_init() __asm__("__GLOBAL__sub_I_PicassoBootState.cpp");\n'
            'extern "C" void call_exported_initializer() { explicit_init(); }\n')
        caller_obj = out / 'caller.o'
        subprocess.run([clang, '-target', 'arm64-apple-ios17.0', '-fno-exceptions', '-fno-rtti',
                        '-c', str(caller), '-o', str(caller_obj)], check=True, capture_output=True)
        subprocess.run([str(llvm / ('ld64.lld' + suffix)), '-arch', 'arm64', '-platform_version',
                        'ios', '17.0', '17.0', '-dylib', '-o', str(out / 'initializer.dylib'),
                        str(late), str(caller_obj)], check=True, capture_output=True)
        passed('separate object links to exported initializer with no unresolved symbols')
        reject('raw automatic initializer rejected as late', lambda: builder.initializer_state(
            obj, native, '__GLOBAL__sub_I_PicassoBootState.cpp', False))
        reject('missing exact initializer rejected', lambda: builder.initializer_state(
            obj, native, '__GLOBAL__sub_I_Wrong.cpp', True))

        record = {'file': 'original.cpp', 'arguments': ['clang', '-xc++', '-c', 'original.cpp', '-o', 'original.o',
            '-MJ', 'original.json', '-MF', 'original.d', '-MMD', '-Xclang', '-Wold', '-Werror=old',
            '-DKERNEL', '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE', '-std=gnu++2b', '-fsized-deallocation',
            '-fapple-kext', '-target', 'arm64-apple-ios17.0'], 'directory': str(out)}
        command = builder.compile_command(record, out / 'new.cpp', out / 'new.o', out)
        assert all(x not in command for x in ('original.cpp', 'original.o', 'original.json', 'original.d', '-MMD', '-Wold'))
        assert all(x in command for x in ('-DKERNEL', '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE',
                                         '-std=gnu++2b', '-fsized-deallocation', '-fapple-kext', '-Werror'))
        passed('compile-record ABI retained, source/output replaced, original metadata protected')
        reject('ambiguous recorded source rejected', lambda: builder.compile_command(
            {**record, 'file': 'absent.cpp'}, cpp, obj, out))

        data = args.kernelcache.read_bytes()
        original = native.macho(data)
        exports = native.exports(data, original)
        targets = {address: name for name, address in binding.STOCK_BINDINGS.items()}
        slots = []
        for index in range(171):
            address = int(bindings['vtable_slots'][index]['original_target'], 0)
            slots.append(binding.NATIVE_OVERRIDES.get(index) or targets.get(address) or exports[address][0])
        initializer = binding.NATIVE_INITIALIZER
        schema = '.section __TEXT,__text,regular,pure_instructions\n.p2align 2\n'
        for name in binding.NATIVE_OVERRIDES.values():
            schema += f'.globl {name}\n{name}:\nret\n'
        schema += f'{initializer}:\nret\n.section __DATA,__const\n.p2align 3\n'
        schema += f'.globl {binding.NATIVE_VTABLE_SYMBOL}\n{binding.NATIVE_VTABLE_SYMBOL}:\n.quad 0, 0\n'
        schema += ''.join(f'.quad {name}\n' for name in slots) + '.quad 0\n'
        schema += '.globl __ZN15PicassoWatchdog10superClassE\n__ZN15PicassoWatchdog10superClassE:\n'
        schema += '.quad __ZN10IOWatchdog10gMetaClassE\n'
        schema += '.quad __ZN10IOWatchdogC2EPK11OSMetaClass, __ZN10IOWatchdogD2Ev\n'
        schema += '.quad _picasso_original_iowatchdog_policy_start, _picasso_original_iowatchdog_policy_check\n'
        schema += '.quad __ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient\n'
        schema += f'.section __DATA,__mod_init_func,mod_init_funcs\n.p2align 3\n.quad {initializer}\n'
        asm = out / 'schema.S'; asm.write_text(schema)
        obj, late = out / 'schema.o', out / 'schema.late.o'; assemble(asm, obj)
        binding.audit_object(obj, args.native_prelink_dir, bindings)
        builder.make_late(obj, late, native, objcopy, initializer)
        audit = binding.audit_object(late, args.native_prelink_dir, bindings)
        assert audit['automatic_initializers_removed'] and audit['initializer_symbol_global']
        passed('171-slot original ABI audit before and after real objcopy')
        for name, mutated in [
            ('wrong inherited stop', schema.replace('.quad ' + slots[87] + '\n', '.quad __ZN9IOService4stopEPS_\n', 1)),
            ('extra virtual slot', schema.replace('.quad 0\n.globl __ZN15PicassoWatchdog10superClassE',
                  '.quad _extra_virtual\n.globl __ZN15PicassoWatchdog10superClassE')),
            ('extra initializer', schema + '.quad ' + initializer + '\n')]:
            bad = out / (name.replace(' ', '-') + '.S'); bad.write_text(mutated)
            badobj = bad.with_suffix('.o'); assemble(bad, badobj)
            reject(name, lambda: binding.audit_object(badobj, args.native_prelink_dir, bindings))

        # Run the entire production builder. Only the input sources/compile record
        # are synthetic assembler schemas; xcrun ar maps to the installed llvm-ar.
        tree = out / 'tree'
        for part in ('scripts', 'kits/NativePrelink', 'kits/picasso'):
            (tree / part).mkdir(parents=True)
        shutil.copy2(ROOT / 'scripts/prepare-picasso-watchdog-bindings.py', tree / 'scripts')
        shutil.copy2(args.native_prelink_dir / 'native_prelink.py', tree / 'kits/NativePrelink')
        (tree / 'board.toml').write_text('[kernel.prelink]\nslide="-0x1000000"\nkernelcache_sha256="' +
                                       binding.ORIGINAL_SHA256 + '"\n')
        (tree / 'kits/picasso/PicassoWatchdog.cpp').write_text(schema)
        (tree / 'kits/picasso/PicassoPmgr.cpp').write_text('.text\n.globl _sheng_pmgr_run\n_sheng_pmgr_run:\nret\n')
        boot_schema = '.text\n__GLOBAL__sub_I_PicassoBootState.cpp:\nret\n'
        boot_schema += '.section __DATA,__mod_init_func,mod_init_funcs\n.p2align 3\n.quad __GLOBAL__sub_I_PicassoBootState.cpp\n'
        (tree / 'kits/picasso/PicassoBootState.cpp').write_text(boot_schema)
        record_path = tree / builder.RECORD; record_path.parent.mkdir(parents=True)
        synthetic = {'file': 'record-source.cpp', 'directory': str(tree),
                     'arguments': [clang, '-target', 'arm64-apple-ios17.0', '-x', 'assembler', '-c',
                                   'record-source.cpp', '-o', 'record-source.o']}
        record_path.write_text(json.dumps(synthetic) + ',')
        archive = tree / 'build/sheng-platform-power/libShengPlatformPower.a'
        archive.parent.mkdir(parents=True)
        archive.write_bytes(b'previous archive retained')
        real_run = builder.run
        def local_run(command, log, cwd=None):
            if command[:2] == ['xcrun', 'ar']:
                command = [ar] + command[2:]
            return real_run(command, log, cwd)
        environment = {'UNIXNU_DEVICE': str(tree), 'UNIXNU_KERNELCACHE': str(args.kernelcache.resolve()),
                       'PATH': str(llvm) + os.pathsep + os.environ['PATH']}
        with patch.object(builder, 'ROOT', tree), patch.object(builder, 'run', local_run), patch.dict(os.environ, environment):
            builder.main()
            built = json.loads((archive.parent / 'build-report.json').read_text())
            assert built['complete'] and built['members'] == ['PicassoPmgr.o', 'PicassoWatchdog.late.o',
                                                            'PicassoBootState.late.o', 'watchdog-bindings.o']
            assert Path(built['previous_archive']).read_bytes() == b'previous archive retained'
            passed('complete archive build has exactly four qualified objects and preserves previous archive')
            prior = archive.read_bytes()
            with patch.dict(os.environ, {'UNIXNU_KERNELCACHE': str(corrupted)}):
                reject('full builder fails closed on wrong image', builder.main)
            assert archive.read_bytes() == prior
            assert not json.loads((archive.parent / 'build-report.json').read_text())['complete']
            passed('failed rebuild preserves last qualified archive and reports failure')
        report['pass'] = True
        print(f'PASS: {len(report["checks"])} builder/ABI checks')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
