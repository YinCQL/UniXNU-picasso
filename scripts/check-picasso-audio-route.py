#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the exact offline route adaptation, signed payload and kernel helpers."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import tempfile
import zlib

import picasso_audio_route as route
import picasso_audio_payload as payload
from picasso_ui_trust import library_identity

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=route.DEFAULT)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    work = a.work.resolve()
    runs = []

    def passed(name):
        runs.append(dict(name=name, passed=True))
        print(name, 'PASS', flush=True)

    def reject(name, fn):
        try:
            fn()
        except (ValueError, AssertionError):
            passed(name)
        else:
            raise AssertionError('Accepted invalid input: ' + name)

    stock = (a.reference / 'VirtualAudio').read_bytes()
    info = (a.reference / 'Info.plist').read_bytes()
    candidate = (work / 'audio-hal/VirtualAudio.plugin/VirtualAudio').read_bytes()
    expected, report = route.prepare_bytes(stock, info)
    assert expected == candidate and route.verify(work/'audio-hal') == report
    assert library_identity(candidate) == report['identity']
    assert len(candidate) == len(stock)
    assert struct.unpack_from('<I', candidate, 0x32aef0)[0] == 0x52800023
    changed = {i for i in range(len(stock)) if stock[i] != candidate[i]}
    assert 0x32aef0 in changed and changed <= {0x32aef0} | set(range(route.DIGEST, route.DIGEST+32))
    passed('exact-instruction-and-signature-only')
    for offset in [0, 0x120d0, 0x32aef0, 0x634000, len(stock)-1]:
        bad = bytearray(stock)
        bad[offset] ^= 1
        reject('reject-stock-' + hex(offset), lambda: route.prepare_bytes(bytes(bad), info))
    reject('reject-already-patched', lambda: route.prepare_bytes(candidate, info))
    reject('reject-truncated-stock', lambda: route.prepare_bytes(stock[:-1], info))
    reject('reject-info', lambda: route.prepare_bytes(stock, info + b' '))
    with tempfile.TemporaryDirectory(dir=out) as temporary:
        temp = Path(temporary)
        bundle = temp/'VirtualAudio.plugin'
        bundle.mkdir()
        (bundle/'Info.plist').write_bytes(info)
        for offset in [route.SITE, route.DIGEST, 0x1000, 0x634000, len(candidate)-1]:
            bad = bytearray(candidate)
            bad[offset] ^= 1
            (bundle/'VirtualAudio').write_bytes(bad)
            reject('reject-output-' + hex(offset), lambda: route.verify(temp))
        # Re-sign an unrelated changed code page. A valid code signature alone
        # must not make this file an accepted route adaptation.
        bad = bytearray(candidate)
        bad[0x2000] ^= 1
        digest = route.HASHES + 2*32
        bad[digest:digest+32] = hashlib.sha256(bad[0x2000:0x3000]).digest()
        library_identity(bytes(bad))
        (bundle/'VirtualAudio').write_bytes(bad)
        reject('reject-signed-unrelated-change', lambda: route.verify(temp))
        board = (ROOT/'board.toml').read_text()
        for old, new in [('iPhone10,3', 'iPhone10,6'), ('20H392', '20H391'), ('d22ap', 'd221ap')]:
            (temp/'board.toml').write_text(board.replace(old, new))
            reject('reject-target-'+new, lambda: route.require_target(temp))
    trust, _, identity = payload.make(work)
    payload.verify(trust, work)
    assert trust == (work/'audio-hal/audio-trust.bundle').read_bytes()
    count, _, _, _, start = struct.unpack_from('<5I', trust)
    assert count == 4
    version, _, entries = struct.unpack_from('<I16sI', trust, start)
    assert version == 2 and entries == 2
    hashes = [trust[start+24+i*24:start+44+i*24] for i in range(entries)]
    assert hashes == sorted([bytes.fromhex(identity['cdhash']), bytes.fromhex(report['identity']['cdhash'])])
    for i in [0, 24, start+24, len(trust)-1]:
        bad = bytearray(trust)
        bad[i] ^= 1
        reject('reject-trust-'+str(i), lambda: payload.verify(bytes(bad), work))
    passed('exact-hal-and-route-trust')
    builder = payload.load(ROOT/'scripts/build-picasso-audio-hal.py')
    files = [(n, m, (work/'audio-hal'/n.split('/',1)[1]).read_bytes()) for n,m in builder.AUDIO_FILES.items()]
    blob = builder.pack(files)
    assert blob == (work/'audio-hal/audio.addon').read_bytes()
    assert struct.unpack_from('<IIQ', blob, 8) == (4,0,len(blob))
    cursor = 24
    for name, mode, data in files:
        n, m, size, compressed = struct.unpack_from('<IIQQ', blob, cursor)
        cursor += 24
        assert blob[cursor:cursor+n] == name.encode()+bytes(1) and m == mode and size == len(data)
        cursor += (n+7)&~7
        decoder = zlib.decompressobj()
        assert decoder.decompress(blob[cursor:cursor+compressed]) == data
        assert decoder.eof and not decoder.unused_data
        cursor += (compressed+7)&~7
    assert cursor == len(blob)
    for name, altered in [('missing',files[:-1]),('extra',files+[files[0]]),('order',files[::-1]),
                          ('path',[('audio/../x',0o644,b'x')]+files[1:]),
                          ('mode',[(files[0][0],0o755,files[0][2])]+files[1:])]:
        reject('reject-addon-'+name, lambda: builder.pack(altered))
    passed('signed-files-match-compressed-addon')
    patch = ROOT/'patches/picasso-audio-route-xnu.patch'
    added = '\n'.join(line[1:] for line in patch.read_text().splitlines()
                      if line.startswith(('+',' ')) and not line.startswith('+++'))+'\n'
    def extract(name):
        start = added.index(name+'(')
        start = added.rfind('static ',0,start)
        return added[start:added.index('\n}',start)+2]+'\n'
    (out/'route-helpers.inc').write_text(''.join(extract(n) for n in
        ['picasso_audio_addon_entry','picasso_audio_route_mount','picasso_audio_prepare']))
    for sanitized in [False, True]:
        suffix = '-asan' if sanitized else ''
        flags = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else []
        exe = out/('route'+suffix)
        for name, command in [('compile'+suffix,['/usr/bin/clang++-18','-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,
                '-I',str(out),str(ROOT/'scripts/tests/picasso-audio-route-host.cpp'),'-o',str(exe)]),
                ('mount-whitelist'+suffix,[str(exe)])]:
            proc = subprocess.run(command,capture_output=True,text=True,timeout=120)
            (out/(name+'.log')).write_text(proc.stdout+proc.stderr)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            passed(name)
    paths = ['scripts/picasso_audio_route.py','scripts/picasso_audio_payload.py','scripts/build-picasso-audio-hal.py',
             'scripts/picasso_ui_trust.py','scripts/check-picasso-audio-route.py','scripts/tests/picasso-audio-route-host.cpp',
             'patches/picasso-audio-route-xnu.patch','board.toml']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,
        route=report,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in paths}),indent=2)+'\n')


if __name__ == '__main__':
    main()
