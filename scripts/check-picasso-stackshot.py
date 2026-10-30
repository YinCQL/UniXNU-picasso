#!/usr/bin/env python3
"""Check the actual CDC stackshot exporter, framing and matching XNU decoder.

Uses a host USB/clock model, normal + ASan C execution, and generated kcdata
fixtures. This is not a kernel SDK build or hardware/real-stackshot acceptance.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def item(kind, payload=b'', flags=0):
    payload += bytes(-len(payload) % 16)
    return struct.pack('<IIQ', kind, len(payload), flags) + payload


def frame(blob):
    rows = [f'PICASSO_STACKSHOT begin v=1 bytes={len(blob)} encoding=hex chunk=96 crc=crc32-ieee']
    rows += [f'PICASSO_STACKSHOT data offset={offset:08x} hex={blob[offset:offset + 96].hex()}' for offset in range(0, len(blob), 96)]
    rows += [f'PICASSO_STACKSHOT end bytes={len(blob)} crc32={zlib.crc32(blob):08x}']
    return ('\r\n'.join(rows) + '\r\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('clang'))
    args = parser.parse_args()
    if not args.cc: parser.error('Installed Clang required; use --cc for a compiler outside PATH')
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    decoder = load(root / 'scripts/decode-picasso-stackshot.py', 'decoder')
    source = (args.xnu / 'bsd/dev/picasso_usb.c').read_text()
    block = re.findall(r'/\* PICASSO_STACKSHOT_EXPORT_BEGIN.*?/\* PICASSO_STACKSHOT_EXPORT_END \*/', source, re.S)
    assert len(block) == 1
    def function(name):
        return re.findall(r'static void\n' + name + r'\(.*?\n}', source, re.S)[0]
    extracted = function('pup_put') + '\n' + function('pup_puts') + '\n' + block[0]
    (out / 'stackshot-export.inc').write_text(extracted)
    shutil.copyfile(root / 'scripts/tests/picasso-stackshot-export.c', out / 'fixture.c')
    model = (args.xnu / 'osfmk/arm/model_dep.c').read_text()
    hook = model.index('picasso_usb_panic_stackshot(stackshot_begin_loc, (unsigned)bytes_traced);')
    assert model.index('if (bytes_traced > 0 && !err)') < model.index('debug_buf_ptr += bytes_traced;') < hook
    assert model.index('panic_info->eph_stackshot_len = bytes_traced;') < hook < model.index('panic_info->eph_other_log_offset = PE_get_offset_into_panic_region(debug_buf_ptr);', hook)
    assert all(term not in block[0] for term in ('IOMalloc', 'copyin(', 'backtrace_user(', 'watchdog_pet(', 'usb_wr(', 'paniclog_append'))
    report = {'scope': __doc__, 'pass': False, 'runs': [], 'parser_cases': [],
              'source_sha256': {p: hashlib.sha256((args.xnu / p).read_bytes()).hexdigest() for p in ('bsd/dev/picasso_usb.c', 'osfmk/arm/model_dep.c')},
              'export_sha256': hashlib.sha256(extracted.encode()).hexdigest()}
    def reject(name, data, unpack=False):
        try: (decoder.unpack_kcdata if unpack else decoder.extract)(data)
        except (ValueError, zlib.error): report['parser_cases'].append(name)
        else: raise AssertionError('Accepted invalid ' + name)
    try:
        # Run the actual XNU flag helper instead of assuming flags=0. Its
        # allocation sink is modeled; compression below uses Python zlib.
        kcsource = (args.xnu / 'osfmk/kern/kern_cdata.c').read_text()
        kcheader = (args.xnu / 'osfmk/kern/kcdata.h').read_text()
        flag_helper = re.findall(r'kern_return_t\nkcdata_get_memory_addr\(.*?\n}', kcsource, re.S)
        assert len(flag_helper) == 1
        constants = '\n'.join(re.findall(r'^#define KCDATA_FLAGS_STRUCT_(?:HAS_PADDING|PADDING_MASK) .*$', kcheader, re.M))
        (out / 'kcdata-header.inc').write_text(constants + '\n' + flag_helper[0] + '\n')
        shutil.copyfile(root / 'scripts/tests/picasso-kcdata-header.c', out / 'kcdata-header.c')
        header_exe = out / 'kcdata-header.exe'
        result = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', 'kcdata-header.c', '-o', str(header_exe)], cwd=out, capture_output=True, text=True)
        (out / 'kcdata-header-compile.txt').write_text(result.stdout + result.stderr)
        assert result.returncode == 0
        headers = out / 'kcdata-headers.bin'
        subprocess.run([str(header_exe), str(headers)], check=True, timeout=10)
        native = headers.read_bytes(); assert len(native) == 64
        report['kcdata_flag_helper_sha256'] = hashlib.sha256(flag_helper[0].encode()).hexdigest()
        payload = native[48:64] + b'fixture_marker'.ljust(32, b'\0') + struct.pack('<I', 88) + bytes(12)
        end = item(decoder.BUFFER_END)
        raw = native[16:32] + payload + end
        # finalize_kcdata finishes compression on stackshot_out_flags, then
        # writes BUFFER_END outside the compressed stream.
        body = payload
        expected_unpacked = item(decoder.STACKSHOT) + body + end
        packed = zlib.compress(body)
        comp = native[:16]
        for name, value in [('kcd_c_type', 1), ('kcd_c_totalout', len(packed)), ('kcd_c_totalin', len(body))]:
            comp += native[32:48] + name.encode().ljust(32, b'\0') + struct.pack('<Q', value) + bytes(8)
        # kcdata_memory_get_used_bytes counts an extra 16-byte end slot after
        # the compressed bytes. It is not part of the zlib stream.
        comp += native[16:32] + packed + end
        assert decoder.unpack_kcdata(comp) == expected_unpacked
        assert decoder.unpack_kcdata(raw) == raw
        (out / 'compressed.kcdata').write_bytes(comp)
        (out / 'raw.kcdata').write_bytes(raw)
        default_input = out / 'input.bin'; default_input.write_bytes(bytes(range(256)) * 735 + bytes(range(31)))
        for asan in (False, True):
            exe = out / ('export-asan.exe' if asan else 'export.exe')
            command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O0', '-g', 'fixture.c', '-o', str(exe)]
            if asan: command += ['-fsanitize=address', '-fno-omit-frame-pointer']
            result = subprocess.run(command, cwd=out, capture_output=True, text=True)
            (out / (exe.stem + '-compile.log')).write_text(result.stdout + result.stderr)
            assert result.returncode == 0, 'Compile failed: ' + exe.stem
            entry = {'asan': asan, 'command': command, 'cases': []}; report['runs'].append(entry)
            for case in range(19):
                output = out / f'{exe.stem}-{case}.log'
                result = subprocess.run([str(exe), str(case), str(default_input), str(output)], capture_output=True, text=True)
                (out / f'{exe.stem}-{case}.result.txt').write_text(result.stdout + result.stderr)
                assert result.returncode == 0, f'{exe.stem} case {case}: {result.stderr}'
                data = output.read_bytes()
                assert max(map(len, data.splitlines()), default=0) < 256
                if case in (0, 12, 13, 14, 15, 16, 18):
                    assert decoder.extract(data)[0] == default_input.read_bytes()
                    assert data.count(b'PICASSO_STACKSHOT begin ') == 1
                else: reject(f'{exe.stem}-{case}', data)
                if case == 13: assert data.startswith(b'PREEXISTING PANIC TEXT\r\n')
                entry['cases'].append(case)
            for size in (1, 95, 96, 97, 1536, 1537, 512 * 1024):
                data = bytes(i % 251 for i in range(size)); fixture = out / f'bytes-{size}.bin'; fixture.write_bytes(data)
                output = out / f'{exe.stem}-bytes-{size}.log'
                result = subprocess.run([str(exe), '0', str(fixture), str(output)], capture_output=True, text=True)
                assert result.returncode == 0 and decoder.extract(output.read_bytes())[0] == data
                entry.setdefault('sizes', []).append(size)
            output = out / f'{exe.stem}-kcdata.log'
            result = subprocess.run([str(exe), '0', str(out / 'compressed.kcdata'), str(output)], capture_output=True, text=True)
            assert result.returncode == 0 and decoder.unpack_kcdata(decoder.extract(output.read_bytes())[0]) == expected_unpacked
        valid = frame(default_input.read_bytes()); rows = valid.splitlines(keepends=True)
        reject('missing-footer', b''.join(rows[:-1]))
        reject('missing-chunk', b''.join(rows[:2] + rows[3:]))
        reject('duplicate-chunk', b''.join(rows[:2] + rows[1:]))
        reject('out-of-order', b''.join(rows[:1] + rows[2:3] + rows[1:2] + rows[3:]))
        reject('wrong-crc', valid.replace(b'crc32=', b'crc32=0'))
        corrupt = valid.replace(b'hex=000102', b'hex=ff0102', 1); reject('changed-byte', corrupt)
        reject('wrong-length', valid.replace(f'end bytes={len(default_input.read_bytes())}'.encode(), b'end bytes=1'))
        reject('latest-incomplete', valid + b''.join(rows[:-1]))
        assert decoder.extract(valid + b''.join(rows[:-1]), 0)[0] == default_input.read_bytes()
        reject('aborted-after-data', b''.join(rows[:-1]) + b'PICASSO_STACKSHOT aborted queued=1 reason=transport-budget\n')
        reject('oversized-begin', valid.replace(b'bytes=188191', b'bytes=999999999', 1))
        reject('unknown-version', valid.replace(b'begin v=1', b'begin v=2', 1))
        reject('after-footer', valid + rows[1])
        assert decoder.extract(b'ordinary panic text\r\n' + valid + b'reset notice\r\n')[0] == default_input.read_bytes()
        assert decoder.extract(valid + valid)[0] == default_input.read_bytes()
        reject('truncated-kcdata', comp[:10], True)
        reject('truncated-zlib', comp[:-19], True)
        bad = comp.replace(struct.pack('<Q', len(body)), struct.pack('<Q', decoder.MAX_EXPANDED + 1), 1)
        reject('decompression-limit', bad, True)
        # Zero flags remain compatible with synthetic roots from kcdata.py.
        assert decoder.unpack_kcdata(comp[:8] + bytes(8) + comp[16:]) == expected_unpacked
        reject('unexpected-root-flags', comp[:8] + struct.pack('<Q', 1) + comp[16:], True)
        reject('bad-descriptor-flags', comp[:24] + struct.pack('<Q', 1) + comp[32:], True)
        reject('missing-buffer-end', comp[:-16], True)
        reject('extra-buffer-bytes', comp + end, True)
        # Change lengths consistently so only zlib integrity/EOF rejects them.
        packed_start = len(comp) - 16 - len(packed)
        packed_header = comp[:packed_start]
        truncated = packed_header.replace(struct.pack('<Q', len(packed)), struct.pack('<Q', len(packed) - 3), 1) + packed[:-3] + end
        reject('zlib-premature-eof', truncated, True)
        extra = packed_header.replace(struct.pack('<Q', len(packed)), struct.pack('<Q', len(packed) + 1), 1) + packed + b'x' + end
        reject('zlib-trailing-data', extra, True)
        bomb_body = zlib.compress(bytes(decoder.MAX_EXPANDED + 1))
        bomb_header = packed_header.replace(struct.pack('<Q', len(packed)), struct.pack('<Q', len(bomb_body)), 1)
        reject('actual-expansion-limit', bomb_header + bomb_body + end, True)
        # Exercise the matching original parser and both complete CLI paths.
        upstream = args.xnu / 'tools/lldbmacros/kcdata.py'
        for name in ('compressed', 'raw'):
            result = subprocess.run([sys.executable, '-X', 'utf8', str(upstream.resolve()), str(out / (name + '.kcdata'))], capture_output=True, text=True, encoding='utf-8')
            (out / (name + '-original-decoder.txt')).write_text(result.stdout + result.stderr, encoding='utf-8')
            assert result.returncode == 0 and json.loads(result.stdout)['kcdata_stackshot']['fixture_marker'] == 88
            log = out / (name + '-framed.log'); log.write_bytes(frame((out / (name + '.kcdata')).read_bytes()))
            result = subprocess.run([sys.executable, '-X', 'utf8', str(root / 'scripts/decode-picasso-stackshot.py'), '--log', str(log), '--output', str(out / (name + '-decoded')), '--kcdata-script', str(upstream.resolve())], capture_output=True, text=True, encoding='utf-8')
            (out / (name + '-cli.txt')).write_text(result.stdout + result.stderr, encoding='utf-8')
            assert result.returncode == 0, result.stderr
        report['original_decoder_sha256'] = hashlib.sha256(upstream.read_bytes()).hexdigest()
        report['pass'] = True
        print('PASS: normal + ASan exporter, strict framing, compression bounds and original XNU decoder')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
