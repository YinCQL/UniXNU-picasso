#!/usr/bin/env python3
"""Prelink Picasso, adapting only watchdogd's exact service-class policy atom.

The original daemon, its entitlements, Sandbox evaluator, UserClient filter,
and all executable bytes remain unchanged. This is an explicit board policy
change, not a MAC-check bypass or an Apple class-name override.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
SLIDE = -0x1000000
SANDBOX = 'com.apple.security.sandbox'
OLD_CLASS = b'AppleARMWatchdogTimer'
NATIVE_CLASS = b'PicassoWatchdogDriver'
ATOM_VA = 0xfffffff005838818
OLD_ATOM = b'\x1a\x00\x54' + OLD_CLASS + b'\x0f\x00\x0f\x0a\x00\x00\x00\x00'
NEW_ATOM = b'\x1a\x00\x54' + NATIVE_CLASS + b'\x0f\x00\x0f\x0a\x00\x00\x00\x00'
CONTEXT_VA = 0xfffffff0058387d0
CONTEXT_SIZE = 0xc8


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_at(native, data, image, address, size):
    offset = native.backed_offset(image['segments'], address, size, len(data))
    return data[offset:offset + size]


def qualify_native(native, data, image):
    """Reject a stale/native-less kernel before changing any policy bytes."""
    exports = native.exports(data, image)
    names = {}
    for address, symbols in exports.items():
        for name in symbols:
            require(name not in names, 'Duplicate linked symbol: ' + name)
            names[name] = address
    prefix = '21' + NATIVE_CLASS.decode()
    superclass = '__ZN' + prefix + '10superClassE'
    required = ['__ZTV' + prefix, '__ZNK' + prefix + '12getMetaClassEv',
                '__ZN' + prefix + '13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient',
                superclass, '__ZN' + prefix + '10gMetaClassE']
    require(all(name in names for name in required), 'Missing actual PicassoWatchdogDriver symbols')
    parent, = struct.unpack('<Q', read_at(native, data, image, names[superclass], 8))
    require(parent == 0xfffffff007b2d268 + SLIDE,
            'Native watchdog does not inherit the retained IOWatchdog metaclass')
    vptr = names['__ZTV' + prefix] + 16
    for slot, name in [(7, required[1]), (140, required[2])]:
        target, = struct.unpack('<Q', read_at(native, data, image, vptr + slot * 8, 8))
        require(target == names[name], 'Linked watchdog vtable does not use its real native class/client')
    require(NATIVE_CLASS + b'\0' in data, 'Missing actual native metaclass name')


def adapt_policy(native, original, candidate, plan):
    """Return a new candidate/report; never mutate the source kernelcache."""
    require(sha(original) == ORIGINAL_SHA256, 'Unaudited original kernelcache')
    require(plan.get('original_sha256') == ORIGINAL_SHA256 and plan.get('slide') == SLIDE,
            'Prelink identity or slide mismatch')
    require(SANDBOX in plan.get('components', []), 'Sandbox component is not selected')
    require(plan.get('candidate_sha256') == sha(candidate), 'Stale prelink report')
    require('picasso_watchdog_policy' not in plan, 'Watchdog policy already adapted')
    require(len(OLD_CLASS) == len(NATIVE_CLASS) == 21 and len(OLD_ATOM) == len(NEW_ATOM) == 32,
            'Policy atom must preserve its encoded length, operators and alignment')
    old_image, new_image = native.macho(original), native.macho(candidate)
    original_atom = read_at(native, original, old_image, ATOM_VA, 32)
    require(original_atom == OLD_ATOM, 'Unexpected original watchdog service-class atom')
    extents = [e for e in plan['extents'] if e['module'] == SANDBOX and e['segment'] == '__TEXT'
               and e['va'] <= CONTEXT_VA + SLIDE and
               CONTEXT_VA + SLIDE + CONTEXT_SIZE <= e['va'] + e['size']]
    require(len(extents) == 1, 'Policy atom is not inside the selected Sandbox read-only data')
    extent = extents[0]
    block = read_at(native, candidate, new_image, extent['va'], extent['size'])
    require(block.count(OLD_CLASS) == 1 and block.count(OLD_ATOM) == 1,
            'Ambiguous watchdog service rule in Sandbox')
    context = read_at(native, original, old_image, CONTEXT_VA, CONTEXT_SIZE)
    require(b'/usr/libexec/watchdogd\0' in context and b'IOWatchdogUserClient' in context,
            'Missing neighboring daemon and original client policy atoms')
    require(read_at(native, candidate, new_image, CONTEXT_VA + SLIDE, CONTEXT_SIZE) == context,
            'Watchdog policy data was changed before board adaptation')
    offset = native.backed_offset(new_image['segments'], ATOM_VA + SLIDE, 32, len(candidate))
    require(candidate[offset:offset + 32] == OLD_ATOM, 'Candidate policy atom mismatch')
    result = candidate[:offset] + NEW_ATOM + candidate[offset + 32:]
    # This comparison covers every byte, including all other policies and code.
    restored = result[:offset] + OLD_ATOM + result[offset + 32:]
    require(restored == candidate and len(result) == len(candidate), 'Unexpected extra candidate mutation')
    report = copy.deepcopy(plan)
    report['picasso_watchdog_policy'] = {
        'schema': 1, 'component': SANDBOX, 'scope': 'watchdogd exact service-class atom',
        'old_class': OLD_CLASS.decode(), 'native_class': NATIVE_CLASS.decode(),
        'original_atom_va': hex(ATOM_VA), 'candidate_atom_va': hex(ATOM_VA + SLIDE),
        'file_offset': offset, 'bytes': 32, 'old_atom_hex': OLD_ATOM.hex(),
        'new_atom_hex': NEW_ATOM.hex(), 'context_sha256': sha(context),
        'before_candidate_sha256': sha(candidate), 'after_candidate_sha256': sha(result),
        'only_literal_bytes_changed': True, 'original_source_modified': False,
        'runtime_qualified': False,
    }
    report['candidate_sha256'] = sha(result)
    return result, report


def verify_candidate(native, original, candidate, plan):
    qualify_native(native, candidate, native.macho(candidate))
    rule = plan.get('picasso_watchdog_policy', {})
    require(rule.get('schema') == 1 and rule.get('after_candidate_sha256') == sha(candidate)
            and plan.get('candidate_sha256') == sha(candidate), 'Missing or stale watchdog policy report')
    image = native.macho(candidate)
    offset = native.backed_offset(image['segments'], ATOM_VA + SLIDE, 32, len(candidate))
    require(candidate[offset:offset + 32] == NEW_ATOM, 'Missing native watchdog service rule')
    before = candidate[:offset] + OLD_ATOM + candidate[offset + 32:]
    original_plan = copy.deepcopy(plan)
    del original_plan['picasso_watchdog_policy']
    original_plan['candidate_sha256'] = rule['before_candidate_sha256']
    rebuilt, expected = adapt_policy(native, original, before, original_plan)
    require(rebuilt == candidate and expected == plan, 'Watchdog policy report does not reproduce the candidate')


def build(native, emitter, args):
    require(not args.output.exists() and not args.output.with_suffix('.json').exists(),
            'Refuse to overwrite an existing candidate or report')
    require(args.slide == SLIDE, 'Unaudited Picasso prelink slide')
    original = args.prelinked.read_bytes()
    require(sha(original) == ORIGINAL_SHA256, 'Unaudited original kernelcache')
    initializers = {}
    for request in args.early_security_init:
        identifier, separator, address = request.partition('=')
        require(separator and identifier and identifier not in initializers,
                'Use one BUNDLE=ORIGINAL_VA initializer per module')
        initializers[identifier] = int(address, 0)
    candidate, plan = emitter.emit(args.kernel.read_bytes(), original, args.root, args.slide, initializers)
    qualify_native(native, candidate, native.macho(candidate))
    candidate, plan = adapt_policy(native, original, candidate, plan)
    verify_candidate(native, original, candidate, plan)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as stream:
        stream.write(candidate)
    with args.output.with_suffix('.json').open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(plan, indent=2) + '\n')
    print('Picasso prelink: watchdogd service rule -> ' + NATIVE_CLASS.decode() +
          '; original code/client checks retained; runtime unqualified')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--prelinked', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root', action='append', required=True)
    parser.add_argument('--slide', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--early-security-init', action='append', default=[])
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
    import native_prelink
    import emit_prelink
    build(native_prelink, emit_prelink, args)


if __name__ == '__main__':
    main()
