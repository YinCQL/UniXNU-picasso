#!/usr/bin/env python3
"""Build the EAGL renderer and seal it beside the pinned GPU libraries."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import plistlib
import struct
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REGISTRY_REV = '6af574a14089ccfee87efe230ebcdd8742859813'
REGISTRY_SHA = 'b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3'
SOURCES = ['drivers/NativeGL/eagl/' + n for n in
           ['eagl.c', 'eagl_slots.txt', 'mkeagl.py', 'gl_osmesa.c', 'ca_hooks.c', 'surface_hash.h', 'hakios.gl.h']]
SOURCES += ['scripts/picasso_nativegl.py']
BASE = 'addons/NativeAdrenoMetal.bundle/'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def code_directory(data):
    at, signatures = 32, []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x1d:
            signatures.append(struct.unpack_from('<II', data, at + 8))
        at += size
    if len(signatures) != 1:
        raise ValueError('Require one verified signature')
    offset, length = signatures[0]
    for i in range(struct.unpack_from('>I', data, offset + 8)[0]):
        slot, start = struct.unpack_from('>II', data, offset + 12 + i * 8)
        if slot == 0:
            start += offset
            size = struct.unpack_from('>I', data, start + 4)[0]
            return start, data[start:start + size]
    raise ValueError('Missing CodeDirectory')


def replace_resources(original, before, after):
    from picasso_media_payload import identity
    identity(original)
    start, directory = code_directory(original)
    hash_offset, special = struct.unpack_from('>I', directory, 16)[0], struct.unpack_from('>I', directory, 24)[0]
    if special != 3 or directory[36:40] != bytes([32, 2, 0, 12]):
        raise ValueError('Changed plugin special-slot contract')
    at = start + hash_offset - 3 * 32
    if original[at:at + 32] != hashlib.sha256(before).digest():
        raise ValueError('Plugin does not seal the original resources')
    result = bytearray(original)
    result[at:at + 32] = hashlib.sha256(after).digest()
    result = bytes(result)
    identity(result)
    if result[:at] != original[:at] or result[at + 32:] != original[at + 32:]:
        raise ValueError('Plugin changes outside its resource-seal digest')
    return result, at


def build(work):
    from picasso_media_payload import GPU_ADDON, GPU_SHA, GPU_FILES, identity, unpack
    release = Path(os.environ.get('PICASSO_GPU_RELEASE_DIR', work / 'gpu72'))
    addon = (release / GPU_ADDON).read_bytes()
    if sha(addon) != GPU_SHA:
        raise ValueError('Changed pinned GPU release')
    files = {n: b for n, m, b in unpack(addon) if n in GPU_FILES}
    out = work / 'nativegl'
    out.mkdir(parents=True, exist_ok=True)
    mesa = out / 'libOSMesa.8.dylib'
    mesa.write_bytes(files[BASE + mesa.name])
    xml = out / 'gl.xml'
    if not xml.is_file():
        xml.write_bytes(urllib.request.urlopen(
            'https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/' + REGISTRY_REV + '/xml/gl.xml', timeout=60).read())
    if sha(xml.read_bytes()) != REGISTRY_SHA:
        raise ValueError('Changed pinned GL registry')
    sdk = Path(os.environ.get('PICASSO_GL_SDK', '/home/y/toolchains/audio-sdks/iPhoneOS16.5.sdk'))
    cc = os.environ.get('PICASSO_GL_CC', '/usr/bin/clang-18')
    ld = os.environ.get('PICASSO_GL_LD', '/usr/bin/ld64.lld-18')
    nm = os.environ.get('PICASSO_GL_NM', '/usr/bin/llvm-nm')
    commands, inputs = [], {str(xml): REGISTRY_SHA, str(mesa): sha(mesa.read_bytes())}

    def run(name, command):
        commands.append(command)
        p = subprocess.run(command, capture_output=True, text=True)
        (out / (name + '.log')).write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        return p.stdout

    symbols = run('mesa-exports', [nm, '--defined-only', '--extern-only', str(mesa)])
    exported = out / 'exported.txt'
    exported.write_text('\n'.join(s.split()[-1][1:] for s in symbols.splitlines()
                                  if s.split() and s.split()[-1].startswith('_gl')) + '\n')
    src = ROOT / 'drivers/NativeGL/eagl'
    generated = run('generate', ['python3', '-B', str(src / 'mkeagl.py'), str(src / 'eagl_slots.txt'),
                                 str(xml), str(exported), str(src / 'eagl.c')])
    (out / 'eagl_gen.h').write_text(generated)
    for n in ['eagl', 'gl_osmesa', 'ca_hooks']:
        command = [cc, '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk), '-I', str(out),
                   '-O2', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-Wno-frame-address',
                   '-c', str(src / (n + '.c')), '-o', str(out / (n + '.o'))]
        run(n, command)
        deps = run(n + '-deps', command[:-4] + ['-M', '-MT', n, str(src / (n + '.c'))])
        for name in deps.replace('\\\n', ' ').split()[1:]:
            p = Path(name)
            if p.is_file() and p.is_relative_to(sdk):
                inputs[str(p)] = sha(p.read_bytes())
    exports = ['_hakios_gl_create', '_hakios_gl_destroy', '_hakios_gl_function_refused',
               '_hakios_gl_function_values', '_hakios_gl_library', '_hakios_gl_make_current',
               '_hakios_gl_surface_texture', '_nativegl_text']
    output = out / 'libNativeGL.dylib'
    run('link', [ld, '-arch', 'arm64', '-platform_version', 'ios', '16.0', '16.5', '-dylib',
                 '-install_name', '@loader_path/libNativeGL.dylib', '-syslibroot', str(sdk), '-lSystem', '-lobjc',
                 '-framework', 'CoreFoundation', '-framework', 'IOKit', '-framework', 'IOSurface',
                 *[str(out / (n + '.o')) for n in ['eagl', 'gl_osmesa', 'ca_hooks']], str(mesa),
                 *[arg for n in exports for arg in ['-exported_symbol', n]], '-o', str(output), '-adhoc_codesign'])
    actual = run('exports', [nm, '--defined-only', '--extern-only', str(output)])
    if sorted(s.split()[-1] for s in actual.splitlines() if s.split()) != sorted(exports):
        raise ValueError('Changed renderer exported ABI')
    info = identity(output.read_bytes())
    if info['code_page_bytes'] != 4096 or b'eagl mirror=scissor-isolated publish=after-wait' not in output.read_bytes():
        raise ValueError('Missing renderer signature or repair marker')
    for name in [cc, ld, nm]:
        inputs[name] = sha(Path(name).read_bytes())
    report = dict(passed=True, runtime_qualified=False, identity=info, registry_revision=REGISTRY_REV,
                  registry_sha256=REGISTRY_SHA, generated_sha256=sha(generated.encode()), exports=exports,
                  sources={n: sha((ROOT / n).read_bytes()) for n in SOURCES}, inputs=inputs, commands=commands)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def adapt(work, files):
    from picasso_media_payload import identity
    out = work / 'nativegl'
    report = json.loads((out / 'build-report.json').read_text())
    for n, h in report['sources'].items():
        if sha((ROOT / n).read_bytes()) != h:
            raise ValueError('Stale renderer source: ' + n)
    for n, h in report['inputs'].items():
        if sha(Path(n).read_bytes()) != h:
            raise ValueError('Changed renderer build input: ' + n)
    library = (out / 'libNativeGL.dylib').read_bytes()
    if identity(library) != report['identity'] or report['registry_sha256'] != REGISTRY_SHA:
        raise ValueError('Stale renderer identity or registry')
    data = {n: b for n, m, b in files}
    resources = BASE + '_CodeSignature/CodeResources'
    original = data[resources]
    seal = plistlib.loads(original)
    for n in ['libNativeGL.dylib', 'libOSMesa.8.dylib', 'libvulkan_freedreno.dylib']:
        if seal['files'][n] != hashlib.sha1(data[BASE + n]).digest() or seal['files2'][n]['hash2'] != hashlib.sha256(data[BASE + n]).digest():
            raise ValueError('Changed original GPU resource seal')
    seal['files']['libNativeGL.dylib'] = hashlib.sha1(library).digest()
    seal['files2']['libNativeGL.dylib']['hash2'] = hashlib.sha256(library).digest()
    updated = plistlib.dumps(seal, fmt=plistlib.FMT_XML, sort_keys=False)
    plugin, slot = replace_resources(data[BASE + 'NativeAdrenoMetal'], original, updated)
    replacements = {BASE + 'libNativeGL.dylib': library, resources: updated, BASE + 'NativeAdrenoMetal': plugin}
    result = [(n, m, replacements.get(n, b)) for n, m, b in files]
    return result, dict(build=report, resource_sha256=sha(updated), plugin_resource_digest_offset=slot,
                        plugin_identity=identity(plugin), plugin_code_unchanged=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    a = p.parse_args()
    report = build(a.work.resolve())
    print(json.dumps(dict(passed=True, identity=report['identity'])))


if __name__ == '__main__':
    main()
