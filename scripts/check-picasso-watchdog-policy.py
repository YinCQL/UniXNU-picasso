#!/usr/bin/env python3
"""Verify the delivered watchdog policy, or test its gates with a saved CI kernel.

--verify checks a newly built candidate. Without it, the input is an unadapted
CI kernel; tests use its real Sandbox bytes plus a linked ARM64 native-class
schema. These host checks do not execute Sandbox or qualify a phone boot.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--native-prelink-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    policy = load(ROOT / 'scripts/prelink-picasso.py')
    native = load(args.native_prelink_dir / 'native_prelink.py')
    original, candidate = args.kernelcache.read_bytes(), args.candidate.read_bytes()
    plan = json.loads(args.candidate.with_suffix('.json').read_text(encoding='utf-8'))
    if args.verify:
        policy.verify_candidate(native, original, candidate, plan)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(plan['picasso_watchdog_policy'], indent=2) + '\n')
        print('PASS: actual linked native watchdog class and reproduced policy adaptation')
        return
    if args.llvm_bin is None:
        parser.error('--llvm-bin is required for linked Mach-O gate tests')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    checks = []

    def passed(name):
        checks.append(name)

    def reject(name, operation):
        try:
            operation()
        except ValueError:
            passed(name)
        else:
            raise AssertionError('Expected rejection: ' + name)

    patched, report = policy.adapt_policy(native, original, candidate, plan)
    rule = report['picasso_watchdog_policy']
    offset = rule['file_offset']
    assert candidate[:offset + 3] == patched[:offset + 3]
    assert candidate[offset + 24:] == patched[offset + 24:]
    assert patched[offset + 3:offset + 24] == b'PicassoWatchdogDriver'
    assert len(candidate) == len(patched) and plan['candidate_sha256'] == policy.sha(candidate)
    assert report['original_executable_sha256'] == plan['original_executable_sha256']
    passed('real CI image changes only the 21 service-class literal bytes; all code and other policies identical')
    (out / 'policy-delta.json').write_text(json.dumps(rule, indent=2) + '\n')
    reject('wrong original image', lambda: policy.adapt_policy(native, b'wrong', candidate, plan))
    for key, value in [('slide', 0), ('original_sha256', 'bad'), ('candidate_sha256', 'bad'),
                       ('components', []), ('extents', []), ('picasso_watchdog_policy', {})]:
        reject('reject report ' + key, lambda key=key, value=value:
               policy.adapt_policy(native, original, candidate, {**plan, key: value}))
    for name, where, replacement in [('atom opcode', offset + 2, b'\x55'),
                                      ('neighbor client', offset - 15, b'X'),
                                      ('neighbor daemon', offset - 35, b'X')]:
        broken = candidate[:where] + replacement + candidate[where + 1:]
        reject('reject changed ' + name, lambda broken=broken: policy.adapt_policy(
            native, original, broken, {**plan, 'candidate_sha256': policy.sha(broken)}))
    reject('second adaptation refused', lambda: policy.adapt_policy(native, original, patched, report))
    reject('unadapted artifact fails delivery verification', lambda:
           policy.verify_candidate(native, original, candidate, plan))
    reject('old native class cannot qualify a new policy', lambda:
           policy.qualify_native(native, candidate, native.macho(candidate)))

    # Actual LLVM linked Mach-O, not a fake symbol dictionary: exercise the
    # final-image superclass and dispatch-pointer checks independently of SDK.
    prefix = '21PicassoWatchdogDriver'
    meta = '__ZNK' + prefix + '12getMetaClassEv'
    client = '__ZN' + prefix + '13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient'
    superclass = '__ZN' + prefix + '10superClassE'
    vtable = '__ZTV' + prefix
    assembly = '.section __TEXT,__text,regular,pure_instructions\n.p2align 2\n'
    for name in (meta, client):
        assembly += f'.globl {name}\n{name}:\nret\n'
    assembly += '.section __TEXT,__cstring,cstring_literals\n.asciz "PicassoWatchdogDriver"\n'
    assembly += f'.section __DATA,__const\n.p2align 3\n.globl {vtable}\n{vtable}:\n.quad 0,0\n'
    assembly += ''.join('.quad ' + ({7: meta, 140: client}.get(i, '0')) + '\n' for i in range(171))
    assembly += f'.globl {superclass}\n{superclass}:\n.quad 0xfffffff006b2d268\n'
    assembly += f'.globl __ZN{prefix}10gMetaClassE\n__ZN{prefix}10gMetaClassE:\n.quad 0\n'
    (out / 'native.S').write_text(assembly)
    suffix = '.exe' if os.name == 'nt' else ''
    subprocess.run([str(args.llvm_bin / ('clang' + suffix)), '-target', 'arm64-apple-ios17.0',
                    '-c', str(out / 'native.S'), '-o', str(out / 'native.o')], check=True, capture_output=True)
    subprocess.run([str(args.llvm_bin / ('ld64.lld' + suffix)), '-arch', 'arm64', '-platform_version',
                    'ios', '17.0', '17.0', '-dylib', '-o', str(out / 'native.dylib'),
                    str(out / 'native.o')], check=True, capture_output=True)
    schema = (out / 'native.dylib').read_bytes()
    image = native.macho(schema)
    policy.qualify_native(native, schema, image)
    passed('linked ARM64 native class, real original parent and expected dispatch qualify')
    symbols = {name: address for address, names in native.exports(schema, image).items() for name in names}
    for name, address in [('parent', symbols[superclass]), ('metaclass slot', symbols[vtable] + 16 + 7 * 8),
                          ('client slot', symbols[vtable] + 16 + 140 * 8)]:
        at = native.backed_offset(image['segments'], address, 8, len(schema))
        broken = schema[:at] + bytes(8) + schema[at + 8:]
        reject('reject linked wrong ' + name, lambda broken=broken:
               policy.qualify_native(native, broken, native.macho(broken)))

    # Wrapper/file integration: retain real policy data and emitter arguments;
    # class qualification is a stand-in here, separately tested above.
    native_input = out / 'input-native.macho'
    native_input.write_bytes(schema)
    calls = []
    def emit(data, source, roots, slide, initializers):
        assert data == schema and source == original
        assert roots == plan['components'] and slide == -0x1000000
        assert initializers == {'com.apple.driver.AppleMobileFileIntegrity': 0xfffffff005ab4ae0}
        calls.append('emit')
        return candidate, copy.deepcopy(plan)
    actual_qualify = policy.qualify_native
    def qualify(parser, data, image):
        assert parser is native and data in (candidate, patched)
        calls.append('qualify')
    policy.qualify_native = qualify
    options = SimpleNamespace(kernel=native_input, prelinked=args.kernelcache,
        output=out / 'wrapper/kernel.macho', root=plan['components'], slide=-0x1000000,
        early_security_init=['com.apple.driver.AppleMobileFileIntegrity=0xfffffff005ab4ae0'])
    try:
        policy.build(native, SimpleNamespace(emit=emit), options)
        assert calls == ['emit', 'qualify', 'qualify']
        assert options.output.read_bytes() == patched
        saved = json.loads(options.output.with_suffix('.json').read_text())
        policy.verify_candidate(native, original, patched, saved)
        passed('wrapper forwards real inputs, qualifies before/after, writes consistent kernel/report')
        before = options.output.read_bytes()
        reject('existing output preserved', lambda: policy.build(native, SimpleNamespace(emit=emit), options))
        assert options.output.read_bytes() == before
        for key, value in [('native_class', 'IOService'), ('before_candidate_sha256', 'bad'),
                           ('only_literal_bytes_changed', False)]:
            broken = copy.deepcopy(saved)
            broken['picasso_watchdog_policy'][key] = value
            reject('delivery rejects modified audit ' + key, lambda broken=broken:
                   policy.verify_candidate(native, original, patched, broken))
    finally:
        policy.qualify_native = actual_qualify
    options.output = out / 'refused/kernel.macho'
    reject('stale native driver stops wrapper before any output', lambda:
           policy.build(native, SimpleNamespace(emit=emit), options))
    assert not options.output.exists() and not options.output.with_suffix('.json').exists()
    # The delivered board actually selects this wrapper; no manual post-build step.
    import tomllib
    assert tomllib.loads((ROOT / 'board.toml').read_text(encoding='utf-8'))['kernel']['prelink']['tool'] == 'scripts/prelink-picasso.py'
    passed('board selects the prelink adapter before payload generation')
    result = {'pass': True, 'scope': __doc__, 'checks': checks,
              'candidate_sha256': policy.sha(candidate), 'original_sha256': policy.sha(original),
              'policy_script_sha256': policy.sha((ROOT / 'scripts/prelink-picasso.py').read_bytes()),
              'original_source_unchanged': args.kernelcache.read_bytes() == original}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: {len(checks)} policy, linked-symbol and wrapper checks')


if __name__ == '__main__':
    main()
