#!/usr/bin/env python3
"""Exercise AES identity/181-slot/link/build gates using LLVM Mach-O schemas.

Schemas test the validators and builder flow; they are not production SDK objects.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--native-prelink-dir', type=Path, required=True)
    parser.add_argument('--recorded-bindings', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('aes', ROOT / 'scripts/build-picasso-aes.py')
    aes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(aes)
    platform = aes.load(ROOT / 'scripts/build-picasso-pmgr.py')
    native = aes.load(args.native_prelink_dir / 'native_prelink.py')
    original = args.kernelcache.read_bytes()
    recorded = args.recorded_bindings.read_bytes()
    aliases, slots, expected = aes.qualify_original(native, original, recorded, aes.SLIDE)
    suffix = '.exe' if os.name == 'nt' else ''
    clang = str(args.llvm_bin / ('clang' + suffix))
    ar = str(args.llvm_bin / ('llvm-ar' + suffix))
    ld = str(args.llvm_bin / ('ld64.lld' + suffix))
    checks = ['exact original image/aliases and 181 inherited slots']

    def reject(name, call):
        try:
            call()
        except ValueError:
            checks.append(name)
        else:
            raise AssertionError('Accepted invalid input: ' + name)

    reject('wrong original rejected', lambda: aes.qualify_original(native, b'bad', recorded, aes.SLIDE))
    reject('wrong slide rejected', lambda: aes.qualify_original(native, original, recorded, 0))
    reject('changed alias bytes rejected', lambda: aes.qualify_original(native, original, recorded+b'\n', aes.SLIDE))
    native_names = [group[0].replace('16IOAESAccelerator', '9NativeAES') if i in aes.OVERRIDES else group[0]
                    for i, group in enumerate(expected)]
    initializer = '__GLOBAL__sub_I_NativeAES.cpp'

    def schema(tag, slot_names, superclass='__ZN16IOAESAccelerator10gMetaClassE', local_names=frozenset()):
        source = out / (tag + '.S')
        obj = out / (tag + '.o')
        definitions = sorted({native_names[i] for i in aes.OVERRIDES} | {initializer})
        source.write_text('.text\n' + ''.join(f'.globl {name}\n' +
            (f'.private_extern {name}\n.weak_definition {name}\n' if name in local_names else '') +
            f'{name}:\nret\n' for name in definitions) +
            '.section __DATA,__const\n.p2align 3\n.globl __ZTV9NativeAES\n__ZTV9NativeAES:\n.quad 0,0\n' +
            ''.join(f'.quad {name}\n' for name in slot_names) +
            '.quad 0\n.globl __ZN9NativeAES10superClassE\n__ZN9NativeAES10superClassE:\n.quad '+superclass+'\n' +
            '.section __DATA,__mod_init_func,mod_init_funcs\n.p2align 3\n.quad '+initializer+'\n' +
            '.section __TEXT,__cstring,cstring_literals\n.asciz "NativeAESSoftwareKeysOnly"\n' +
            '.asciz "\x27IOClass\x27         = NativeAES;"\n', encoding='utf-8')
        subprocess.run([clang, '-target', 'arm64-apple-ios17.0', '-c', str(source), '-o', str(obj)], check=True, capture_output=True)
        return obj, source

    obj, schema_source = schema('valid', native_names)
    audit = aes.audit_object(platform, native, obj, original, aliases, slots)
    checks.append('actual LLVM ARM64 object: full 181-slot native/original ABI')
    for tag, index, value in [('wrong-client',169,native_names[3]), ('missing-slot',170,'0')]:
        bad = list(native_names);bad[index]=value
        broken, _ = schema(tag,bad)
        reject(tag+' rejected',lambda broken=broken:aes.audit_object(platform,native,broken,original,aliases,slots))
    broken,_ = schema('extra-slot',native_names+[native_names[3]])
    reject('extra virtual slot rejected',lambda:aes.audit_object(platform,native,broken,original,aliases,slots))
    broken,_ = schema('wrong-parent',native_names,'__ZN9IOService10gMetaClassE')
    reject('wrong native superclass rejected',lambda:aes.audit_object(platform,native,broken,original,aliases,slots))

    symbols = aes.names(native,original,native.macho(original))
    binding_source = out / 'schema-bindings.S'
    targets = {name:aliases[name] if name in aliases else symbols[name] for name in set(native_names) if '9NativeAES' not in name}
    targets['__ZN16IOAESAccelerator10gMetaClassE'] = aes.BASE_META + aes.SLIDE
    binding_source.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name,address in targets.items()))
    binding_obj = out / 'schema-bindings.o'
    subprocess.run([clang,'-target','arm64-apple-ios17.0','-c',str(binding_source),'-o',str(binding_obj)],check=True,capture_output=True)
    linked = out / 'schema.dylib'
    subprocess.run([ld,'-arch','arm64','-platform_version','ios','17.0','17.0','-dylib','-o',str(linked),str(obj),str(binding_obj)],check=True,capture_output=True)
    data = linked.read_bytes()
    plan = {'original_sha256':aes.ORIGINAL_SHA,'slide':aes.SLIDE,'candidate_sha256':aes.sha(data),
            'components':['com.apple.iokit.IOCryptoAcceleratorFamily']}
    aes.verify_kernel(native,original,data,plan,audit,aliases,slots)
    checks.append('real linked ARM64 schema: superclass, aliases, all dispatch slots and personality')
    reject('stale kernel report rejected',lambda:aes.verify_kernel(native,original,data,{**plan,'candidate_sha256':'bad'},audit,aliases,slots))
    reject('missing original component rejected',lambda:aes.verify_kernel(native,original,data,{**plan,'components':[]},audit,aliases,slots))
    bad_audit={**audit,'slot_names':[native_names[1]]+audit['slot_names'][1:]}
    reject('changed linked dispatch rejected',lambda:aes.verify_kernel(native,original,data,plan,bad_audit,aliases,slots))

    # Apple ld localizes the five inline weak methods in the real SDK build.
    # Reproduce that with an actual link; their vtable pointers remain valid.
    local_slots = {111, 117, 168, 171, 172}
    local_obj, _ = schema('inline-local', native_names,
                          local_names={native_names[i] for i in local_slots})
    local_audit = aes.audit_object(platform, native, local_obj, original, aliases, slots)
    local_linked = out / 'inline-local.dylib'
    subprocess.run([ld,'-arch','arm64','-platform_version','ios','17.0','17.0','-dylib',
                    '-o',str(local_linked),str(local_obj),str(binding_obj)],check=True,capture_output=True)
    local_data = local_linked.read_bytes()
    local_image = native.macho(local_data)
    local_exports = aes.names(native, local_data, local_image)
    assert all(native_names[i] not in local_exports for i in local_slots)

    def verify_local(candidate):
        return aes.verify_kernel(native, original, candidate,
            {**plan, 'candidate_sha256': aes.sha(candidate)}, local_audit, aliases, slots)

    report = verify_local(local_data)
    assert report['local_native_dispatch_slots'] == sorted(local_slots)
    checks.append('real link localizes five inline methods; all 181 dispatch slots still verified')
    for index in sorted(local_slots):
        broken = bytearray(local_data)
        at = native.backed_offset(local_image['segments'],
            local_exports['__ZTV9NativeAES'] + 16 + index * 8, 8, len(broken))
        struct.pack_into('<Q', broken, at, local_exports[native_names[0]])
        reject(f'wrong pointer to local method at slot {index} rejected',
               lambda broken=broken: verify_local(bytes(broken)))

    symcmd = next(at for cmd, at, _ in local_image['commands'] if cmd == 2)
    symoff, count, stroff, strsize = struct.unpack_from('<4I', local_data, symcmd + 8)
    strings = local_data[stroff:stroff + strsize]
    records = {}
    for index in range(count):
        at = symoff + index * 16
        strx = struct.unpack_from('<I', local_data, at)[0]
        records[strings[strx:strings.find(b'\0', strx)].decode()] = at
    method_at = records[native_names[111]]
    for tag, kind in [('undefined', 0), ('absolute', 2), ('debug-only', 0x2e)]:
        broken = bytearray(local_data)
        broken[method_at + 4] = kind
        reject(f'{tag} local method rejected', lambda broken=broken: verify_local(bytes(broken)))
    broken = bytearray(local_data)
    struct.pack_into('<I', broken, records[initializer], struct.unpack_from('<I', broken, method_at)[0])
    reject('ambiguous local method rejected', lambda: verify_local(bytes(broken)))
    broken = bytearray(local_data)
    table_at = records['__ZTV9NativeAES']
    broken[method_at + 5] = broken[table_at + 5]
    struct.pack_into('<Q', broken, method_at + 8, local_exports['__ZTV9NativeAES'])
    reject('method symbol in non-executable data rejected', lambda: verify_local(bytes(broken)))
    broken = bytearray(local_data)
    broken[method_at + 5] = 0
    reject('invalid local method section rejected', lambda: verify_local(bytes(broken)))

    # Full builder path with an explicit schema compiler substitution.
    tree = out / 'tree'
    source = tree / 'drivers/NativeAES'
    source.mkdir(parents=True)
    for name in ('NativeAES.cpp','OriginalAESABI.h'):
        shutil.copyfile(ROOT / 'drivers/NativeAES' / name,source/name)
    recorded_path = tree / 'scripts/data/native-aes-original-bindings.S'
    recorded_path.parent.mkdir(parents=True)
    recorded_path.write_bytes(recorded)
    record_path = tree / platform.RECORD
    record_path.parent.mkdir(parents=True)
    record_path.write_text(json.dumps({'file':'unused.cpp','arguments':[clang], 'directory':str(out)}))
    real_load,real_run=aes.load,platform.run
    def host_load(path):
        return platform if path.name=='build-picasso-pmgr.py' else native if path.name=='native_prelink.py' else real_load(path)
    def host_command(record,source,obj,root):
        return [clang,'-target','arm64-apple-ios17.0','-c',str(schema_source),'-o',str(obj)]
    def host_run(command,log,cwd=None):
        command=list(map(str,command))
        if command[:2]==['xcrun','ar']:
            command=[ar]+command[2:]
            if command[1]=='rcs': command.insert(1,'--format=darwin')
        return real_run(command,log,cwd)
    with patch.object(aes,'ROOT',tree),patch.object(aes,'load',host_load),patch.object(platform,'compile_command',host_command),patch.object(platform,'run',host_run),patch.dict(os.environ,{'UNIXNU_DEVICE':str(ROOT),'UNIXNU_KERNELCACHE':str(args.kernelcache.resolve())}):
        aes.main()
        report_path=tree/'build/native-aes/build-report.json'
        report=json.loads(report_path.read_text())
        assert report['complete'] and report['members']==['NativeAES.o','original-bindings.o']
        archive=report_path.with_name('libNativeAES.a')
        old=archive.read_bytes()
        for name in report['members']:
            assert subprocess.check_output([ar,'p',str(archive),name])==archive.with_name(name).read_bytes()
        aes.main()
        second=json.loads(report_path.read_text())
        assert Path(second['previous_archive']).read_bytes()==old
    checks += ['complete builder with schema compiler: exact Darwin members/bytes', 'existing AES archive preserved by move']
    (out/'results.json').write_text(json.dumps({'scope':'Real LLVM ARM64 object/link schemas and builder flow; no new SDK/device execution.',
        'source_sha256':aes.sha((ROOT/'scripts/build-picasso-aes.py').read_bytes()),'checks':checks,'pass':True},indent=2)+'\n')
    print(f'PASS: {len(checks)} original/ABI/link/build checks')


if __name__ == '__main__':
    main()
