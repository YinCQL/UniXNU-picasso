#!/usr/bin/env python3
"""Exercise the actual record verifier with Python SHA256 and host I/O.

Extracts the production verifier and compares its decisions against the build91
version. Uses the production window reader with file-backed transport stand-ins.
No Apple SDK, native IOKit, APFS mounting or phone access. Preserves all outputs.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract(text, name):
    matches = re.findall(r'^static bool ' + name + r'\s*\(.*?^}', text, re.M | re.S)
    if len(matches) != 1:
        raise ValueError('Expected one production helper: ' + name)
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--root-image', type=Path, help='Optional local userdata image, read only')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Requires an installed clang++')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'drivers/NativeUfs/NativeUfs.cpp'
    current = source.read_text(encoding='utf-8')
    old = subprocess.check_output(['git', 'show', '5090cf320e4856b99ba91f867af12e2d713e16e1:drivers/NativeUfs/NativeUfs.cpp'], cwd=ROOT, text=True)
    helper = extract(current, 'equalDigest') + '\n' + extract(current, 'verifyEmbeddedRecord')
    legacy = extract(old, 'verifyEmbeddedRecord').replace('verifyEmbeddedRecord', 'verifyBuild91Record')
    (out / 'actual-verifier.inc').write_text(helper + '\n' + legacy, encoding='utf-8')
    for name in ('RootReadDiagnostics.h', 'RootWindowRead.h', 'RootDiagnostics.h'):
        shutil.copyfile(source.parent / name, out / name)
    bodies = re.findall(r'^IOReturn NativeUfsBlock::doAsyncReadWrite\s*\(.*?^}', current, re.M | re.S)
    if len(bodies) != 1:
        raise ValueError('Expected one production IOKit read method')
    (out / 'actual-read.inc').write_text(bodies[0], encoding='utf-8')
    wrapper = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnIOError=2, kIOReturnNotPrivileged=3;
constexpr uint64_t kDataMax=131072;
#include "RootReadDiagnostics.h"
#include "RootWindowRead.h"
struct vr_view { const uint8_t *hashes; uint64_t block_count, image_bytes; };
using Sha = int (*)(const void *, size_t, uint8_t *);
static struct { Sha sha256; } vr_xnu_crypto;
#include "actual-verifier.inc"
#ifdef _WIN32
#define API extern "C" __declspec(dllexport)
#else
#define API extern "C"
#endif
using Read = int (*)(uint64_t, uint32_t, void *);
API int verify(int prior, const uint8_t *hashes, uint64_t count, uint64_t bytes, uint32_t recordBytes,
               uint64_t index, const void *data, size_t length, Sha sha, char *text) {
  vr_view view{hashes,count,bytes}; vr_xnu_crypto.sha256=sha;
  if(prior) return verifyBuild91Record(&view,recordBytes,index,data,length);
  UfsRecordFailure f{};
  bool ok=verifyEmbeddedRecord(&view,recordBytes,index,data,length,&f);
  if(!ok) {
    formatUfsRecordFailure(text,256,"SystemOS",f);
    formatUfsRecordDigest(text+256,256,"SystemOS",f,true);
    formatUfsRecordDigest(text+512,256,"SystemOS",f,false);
  }
  return ok;
}
API int window(const uint8_t *hashes,uint64_t bytes,uint64_t absolute,uint64_t block,uint64_t count,
               Sha sha,Read read,uint8_t *out,uint64_t *done) {
  vr_view view{hashes,(bytes+kDataMax-1)/kDataMax,bytes}; vr_xnu_crypto.sha256=sha;
  struct Ops {
    vr_view *view; Read reader; uint8_t *out;
    void lock() {} void unlock() {}
    IOReturn read(uint64_t lba,uint32_t blocks,void *data) {return reader(lba,blocks,data)?0:2;}
    IOReturn verify(uint64_t index,const void *data,size_t length) {
      return verifyEmbeddedRecord(view,kDataMax,index,data,length)?kIOReturnSuccess:kIOReturnNotPrivileged;
    }
    uint64_t copy(uint64_t offset,const void *data,uint64_t length) {memcpy(out+offset,data,length);return length;}
  } ops{&view,read,out};
  uint8_t bounce[kDataMax]; UfsVerifiedRecordCache cache{};
  auto result=ufsReadVerifiedRecords(ops,absolute,bytes,block,count,bounce,kDataMax,cache);
  *done=result.done; return result.status;
}
'''
    (out / 'bridge.cpp').write_text(wrapper, encoding='utf-8')
    library = out / ('bridge.dll' if os.name == 'nt' else 'bridge.so')
    report = dict(scope=__doc__, passed=False, cases=[], runs=[], source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())

    def run(name, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (out / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        report['runs'].append(dict(name=name, command=command, returncode=result.returncode))
        if result.returncode:
            raise ValueError(name + ' failed; see preserved log')

    try:
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror']
        run('bridge-build', [args.cxx, *flags, '-shared', *([] if os.name == 'nt' else ['-fPIC']),
                             str(out / 'bridge.cpp'), '-o', str(library)])
        for fixture in ('RootReadDiagnostics', 'RootReadCallback'):
            for sanitized in (False, True):
                name = fixture + ('-asan' if sanitized else '')
                exe = out / (name + ('.exe' if os.name == 'nt' else ''))
                run(name + '-build', [args.cxx, *flags, '-g', '-O1', '-I', str(out),
                    *(['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []),
                    *([] if os.name == 'nt' else ['-pthread']),
                    str(source.parent / ('tests/' + fixture + '.cpp')), '-o', str(exe)])
                run(name + '-run', [str(exe)])
        dll = ctypes.CDLL(str(library))
        ptr, u64, u32, size = ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_size_t
        Sha = ctypes.CFUNCTYPE(ctypes.c_int, ptr, size, ptr)
        Read = ctypes.CFUNCTYPE(ctypes.c_int, u64, u32, ptr)
        dll.verify.argtypes = [ctypes.c_int, ptr, u64, u64, u32, u64, ptr, size, Sha, ptr]
        dll.window.argtypes = [ptr, u64, u64, u64, u64, Sha, Read, ptr, ctypes.POINTER(u64)]

        @Sha
        def sha(data, length, output):
            ctypes.memmove(output, hashlib.sha256(ctypes.string_at(data, length)).digest(), 32)
            return 1

        @Sha
        def failed_sha(data, length, output):
            return 0

        original = bytes(range(256)) * 514
        hashes = b''.join(hashlib.sha256(original[i:i+131072]).digest() for i in range(0, len(original), 131072))

        def case(name, expected, digest=hashes, count=2, total=len(original), record=131072,
                 index=0, data=original[:131072], length=131072, crypto=sha, reason=None):
            old_text, new_text = ctypes.create_string_buffer(768), ctypes.create_string_buffer(768)
            old_result = dll.verify(1, digest, count, total, record, index, data, length, crypto, old_text)
            new_result = dll.verify(0, digest, count, total, record, index, data, length, crypto, new_text)
            assert old_result == new_result == expected, name
            if reason:
                assert reason.encode() in new_text.raw[:256], name
            if expected:
                assert new_text.raw == bytes(768), 'success emitted a failure'
            report['cases'].append(dict(name=name, result='pass', accepted=bool(expected)))
            return new_text.raw

        case('whole record', 1)
        case('short final record', 1, index=1, data=original[131072:], length=512)
        case('bad record length', 0, length=131071, reason='record_length')
        case('bad final length', 0, index=1, data=original[131072:], length=511, reason='record_length')
        case('past last record', 0, index=2, reason='invalid_input')
        case('inconsistent image bounds', 0, total=131072, index=1, reason='record_bounds')
        case('null hash table', 0, digest=None, reason='invalid_input')
        case('null data', 0, data=None, reason='invalid_input')
        case('zero record size', 0, record=0, reason='invalid_input')
        case('unavailable crypto', 0, crypto=Sha(), reason='invalid_input')
        failed = case('crypto failure', 0, crypto=failed_sha, reason='sha256_failed')
        assert b'actual_sha256=unavailable' in failed
        damaged = bytes([original[0] ^ 1]) + original[1:131072]
        failure = case('changed disk bytes', 0, data=damaged, reason='hash_mismatch')
        assert hashlib.sha256(damaged).hexdigest().encode() in failure
        assert hashes[:32].hex().encode() in failure
        case('changed signed hash', 0, digest=bytes([hashes[0] ^ 1])+hashes[1:], reason='hash_mismatch')

        if args.root_image:
            import picasso_target as target
            with args.root_image.open('rb') as disk:
                for window in target.ROOT_WINDOWS:
                    table = (ROOT/'policy/verified-root-picasso'/window['directory']/'manifest.bin').read_bytes()[544:]
                    for index in (0, len(table)//32 - 1):
                        disk.seek(window['offset'] + index * 131072)
                        length = min(131072, window['size'] - index * 131072)
                        data = disk.read(length)
                        assert len(data) == length
                        case(window['name'] + ' actual record ' + str(index), 1, digest=table,
                             total=window['size'], count=len(table)//32, index=index, data=data, length=length)
                    @Read
                    def read(lba, blocks, output):
                        # Translate the physical userdata LBA from the capture
                        # into this local image; never access a device.
                        disk.seek(lba * 4096 - 0x2c0000 * 4096)
                        data = disk.read(blocks * 4096)
                        if len(data) != blocks * 4096:
                            return 0
                        ctypes.memmove(output, data, len(data))
                        return 1
                    for block, count in ((0, 8), (1, 1), (255, 2), (window['size']//512-1, 1)):
                        output, done = ctypes.create_string_buffer(count*512), u64()
                        status = dll.window(table, window['size'], 0x2c0000*4096 + window['offset'], block, count, sha, read, output, ctypes.byref(done))
                        assert status == 0 and done.value == count*512
                        disk.seek(window['offset'] + block*512)
                        assert output.raw == disk.read(count*512)
                        report['cases'].append(dict(name=f'{window["name"]} actual window {block}+{count}', result='pass'))
        report['passed'] = True
    finally:
        (out/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f'PASS: {len(report["cases"])} verifier/window cases and normal/ASan diagnostic fixtures')


if __name__ == '__main__':
    main()
