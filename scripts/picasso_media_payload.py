#!/usr/bin/env python3
"""Compose the pinned GL release beside the existing audio and UI payloads."""
from pathlib import Path
import argparse
import hashlib
import inspect
import json
import os
import struct
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
GPU_ADDON = 'ngpu-addon-ios16-gl-gpu72.bin'
GPU_SHA = '75618b93ca16420f71777f7ee6e733cfedee5cdf23ee3e26a6cc8221f4cead0b'
GPU_TRUST_SHA = '27ea321d7506f6a6f82751b61a7791848ddee938b22357e33f48e136a2ecbc0b'
GPU_FILES = {
    'addons/NativeAdrenoMetal.bundle/Info.plist': 0o644,
    'addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal': 0o755,
    'addons/NativeAdrenoMetal.bundle/_CodeSignature/CodeResources': 0o644,
    'addons/NativeAdrenoMetal.bundle/libNativeGL.dylib': 0o755,
    'addons/NativeAdrenoMetal.bundle/libOSMesa.8.dylib': 0o755,
    'addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib': 0o755,
    'addons/NativeAdrenoMetal.publish': 0o644,
    'addons/NativeAdrenoMetal.clients': 0o644,
}
GPU_ARGS = ['sheng-gpu=4', 'sheng-gpu-metal=1', 'sheng-gpu-addon=1',
            'sheng-dpu-scanout=0', 'sheng-gpu-perf-index=4', 'sheng-gpu-gl=1',
            'glwait=1', 'glhooks=1']
PAYLOAD_MIB = 80
# Physical touch remains enabled. Only synthetic swipe repetition, the redundant
# touch-HID default and the boot-logo hold are omitted from this playback image.
DROP_ARGS = ['offline-hid-swipe-delay=8', 'offline-hid-auto-home-count=8',
             'offline-hid-swipe=8', 'sheng-touch-hid=1', 'picasso-boot-logo-hold=1']


def digest(data):
    return hashlib.sha256(data).hexdigest()


def modules(blob):
    if len(blob) < 4:
        raise ValueError('Truncated trust bundle')
    count = struct.unpack_from('<I', blob)[0]
    if not 1 <= count <= 16 or len(blob) < 4 + 4 * count:
        raise ValueError('Invalid trust module count')
    starts = list(struct.unpack_from('<' + 'I' * count, blob, 4)) + [len(blob)]
    if starts[0] != 4 + 4 * count or any(a >= b for a, b in zip(starts, starts[1:])):
        raise ValueError('Invalid trust module bounds')
    result = [blob[a:b] for a, b in zip(starts, starts[1:])]
    for module in result:
        if len(module) < 24 or struct.unpack_from('<I', module)[0] != 2:
            raise ValueError('Invalid trust module')
        entries = struct.unpack_from('<I', module, 20)[0]
        if len(module) != 24 + 24 * entries:
            raise ValueError('Invalid trust entries')
    return result


def bundle(parts):
    cursor = 4 + 4 * len(parts)
    offsets = []
    for part in parts:
        offsets.append(cursor)
        cursor += len(part)
    return struct.pack('<' + 'I' * (1 + len(parts)), len(parts), *offsets) + b''.join(parts)


def unpack(blob):
    if len(blob) < 24 or blob[:8] != b'NGPUADD2':
        raise ValueError('Invalid addon header')
    count, reserved, total = struct.unpack_from('<IIQ', blob, 8)
    if reserved or total != len(blob) or not 1 <= count <= 32:
        raise ValueError('Invalid addon bounds')
    at, result = 24, []
    for _ in range(count):
        if at + 24 > len(blob):
            raise ValueError('Truncated addon entry')
        n, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        npad, zpad = (n + 7) & ~7, (packed + 7) & ~7
        if not 2 <= n <= 160 or not packed or size > 64 * 1024**2 or at + npad + zpad > len(blob):
            raise ValueError('Invalid addon entry extent')
        raw_name = blob[at:at+n]
        if raw_name[-1:] != b'\0' or b'\0' in raw_name[:-1] or any(blob[at+n:at+npad]):
            raise ValueError('Invalid addon filename')
        name = raw_name[:-1].decode('utf-8')
        if name.startswith('/') or '..' in name.split('/') or name in [x[0] for x in result]:
            raise ValueError('Unsafe or duplicate addon filename')
        at += npad
        decoder = zlib.decompressobj()
        data = decoder.decompress(blob[at:at+packed], size + 1)
        if len(data) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError('Invalid compressed addon entry')
        if any(blob[at+packed:at+zpad]):
            raise ValueError('Nonzero addon padding')
        at += zpad
        result.append((name, mode, data))
    if at != len(blob):
        raise ValueError('Trailing addon bytes')
    return result


def pack(files):
    from picasso_audio_payload import load
    hal = load(ROOT / 'scripts/build-picasso-audio-hal.py')
    expected = list(hal.AUDIO_FILES.items()) + list(GPU_FILES.items())
    if [(name, mode) for name, mode, _ in files] != expected:
        raise ValueError('Require exact ordered audio and GL files')
    body = bytearray()
    for name, mode, data in files:
        encoded, compressed = name.encode() + b'\0', zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(compressed))
        body += encoded + bytes(-len(encoded) % 8)
        body += compressed + bytes(-len(compressed) % 8)
    return b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def identity(data):
    from picasso_ui_trust import library_identity
    # The release has one MH_BUNDLE and three dylibs. Signed bytes stay intact.
    # Keep the existing Data recipe's verifier byte-identical; adapt only its
    # accepted Mach-O kind, and retain every code-page signature check.
    source = inspect.getsource(library_identity)
    if source.count('kind != 6') != 1:
        raise ValueError('Changed signature verifier contract')
    ns = dict(library_identity.__globals__)
    exec(source.replace('kind != 6', 'kind not in (6, 8)'), ns)
    return ns['library_identity'](data)


def kernel_args(base):
    if any(base.count(arg) != 1 for arg in DROP_ARGS):
        raise ValueError('Changed base boot-argument contract')
    result = [arg for arg in base if arg not in DROP_ARGS] + ['picasso-audio-addon=1', *GPU_ARGS]
    names = [arg.split('=', 1)[0] for arg in result]
    if len(names) != len(set(names)) or len(' '.join(result).encode()) > 1000:
        raise ValueError('Duplicate or oversized media boot arguments')
    return result


def make(work):
    import picasso_audio_payload as audio
    import picasso_audio_route as route
    route.require_target(ROOT)
    audio_trust, ui, audio_identity = audio.make(work)
    release = Path(os.environ.get('PICASSO_GPU_RELEASE_DIR', work / 'gpu72'))
    gpu_raw = (release / GPU_ADDON).read_bytes()
    gpu_trust = (release / 'gpu-trust.bundle').read_bytes()
    if digest(gpu_raw) != GPU_SHA or digest(gpu_trust) != GPU_TRUST_SHA:
        raise ValueError('GPU release differs from pinned artifact')
    all_gpu = unpack(gpu_raw)
    gpu_files = [entry for entry in all_gpu if entry[0] in GPU_FILES]
    if [(n, m) for n, m, _ in gpu_files] != list(GPU_FILES.items()):
        raise ValueError('GPU release whitelist differs')
    if gpu_files[-2][2] != b'1\n' or gpu_files[-1][2] != b'backboardd\nSpringBoard\n':
        raise ValueError('Unexpected GPU publication/clients')
    identities = {n: identity(data) for n, mode, data in gpu_files if mode == 0o755}
    release_modules, audio_modules = modules(gpu_trust), modules(audio_trust)
    if len(release_modules) != 3 or len(audio_modules) != 4 or release_modules[:2] != audio_modules[:2]:
        raise ValueError('GPU and audio original trust modules differ')
    trusted = [release_modules[-1][i:i+24] for i in range(24, len(release_modules[-1]), 24)]
    hashes = sorted(bytes.fromhex(info['cdhash']) for info in identities.values())
    if len(hashes) != 4 or len(set(hashes)) != 4 or any(h + bytes([2,0,0,0]) not in trusted for h in hashes):
        raise ValueError('GPU code signature is absent from matching release trust')
    import picasso_nativegl
    gpu_files, gl_report = picasso_nativegl.adapt(work, gpu_files)
    identities = {n: identity(data) for n, mode, data in gpu_files if mode == 0o755}
    hashes = sorted(bytes.fromhex(info['cdhash']) for info in identities.values())
    gpu_module = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL,
        'picasso:gpu:' + ':'.join(h.hex() for h in hashes)).bytes, len(hashes))
    gpu_module += b''.join(h + bytes([2,0,0,0]) for h in hashes)
    hal = audio.load(ROOT / 'scripts/build-picasso-audio-hal.py')
    audio_files = [(n, mode, (work / 'audio-hal' / n.split('/', 1)[1]).read_bytes()) for n, mode in hal.AUDIO_FILES.items()]
    if hal.pack(audio_files) != (work / 'audio-hal/audio.addon').read_bytes():
        raise ValueError('Audio addon is stale')
    files = audio_files + gpu_files
    addon, trust = pack(files), bundle(audio_modules + [gpu_module])
    report = dict(passed=True, runtime_qualified=False, addon_sha256=digest(addon), trust_sha256=digest(trust),
        gpu_release_sha256=GPU_SHA, gpu_release_trust_sha256=GPU_TRUST_SHA, gpu_identities=identities, nativegl=gl_report,
        original_modules_unchanged=True, audio_modules_unchanged=True, exact_owned_entries=7,
        excluded_release_files=[n for n, _, _ in all_gpu if n not in GPU_FILES],
        files={n:dict(mode=m, bytes=len(data), sha256=digest(data)) for n, m, data in files})
    return addon, trust, report, ui, audio_identity


def addon(work):
    return work / 'media/media.addon'


def verify(blob, work):
    expected_addon, expected_trust, report, ui, audio_identity = make(work)
    if blob != expected_trust or addon(work).read_bytes() != expected_addon:
        raise ValueError('Combined media trust or addon differs')
    if json.loads((work / 'media/build-report.json').read_text()) != report:
        raise ValueError('Stale combined media report')
    return ui, audio_identity


def report(work):
    return make(work)[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['prepare', 'check'])
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--tree', type=Path)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    work = a.work.resolve()
    if a.mode == 'prepare':
        raw_addon, trust, result, _, _ = make(work)
        out = work / 'media'
        out.mkdir(exist_ok=True)
        addon(work).write_bytes(raw_addon)
        (out / 'media-trust.bundle').write_bytes(trust)
        (out / 'build-report.json').write_text(json.dumps(result, indent=2) + '\n')
    else:
        import picasso_audio_payload as audio
        import sys
        result = audio.check(work, a.tree.resolve(), a.output.resolve(), profile=sys.modules[__name__])
    print(json.dumps(dict(passed=True, mode=a.mode)))


if __name__ == '__main__':
    main()
