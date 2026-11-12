#!/usr/bin/env python3
"""Prepare the exact audio/GL inputs on a native macOS Compose runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
GPU_URL = 'https://github.com/silenzio111/UniXNU-picasso/releases/download/picasso-gpu-addon-gpu72/'
HEADER_URL = ('https://raw.githubusercontent.com/phracker/MacOSX-SDKs/'
              '041600eda65c6a668f66cb7d56b7d1da3e8bcc93/MacOSX11.3.sdk/'
              'System/Library/Frameworks/CoreAudio.framework/Versions/A/Headers/')
HEADERS = {
    'AudioServerPlugIn.h': (67261, 'd17c4c50a5a68e2deb3947ece9980a26bee7e5417ebbf763bac526625786fa7c'),
    'AudioHardwareBase.h': (79545, '103252d09fd8b87672e8049d6cc700eca725ddf6ca667c3c2270ddd6d41a3af3'),
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(path, url, size, expected):
    if path.exists():
        if path.stat().st_size != size or digest(path) != expected:
            raise ValueError('Cached release input differs: ' + str(path))
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.download')
    request = urllib.request.Request(url, headers={'User-Agent': 'UniXNU-picasso'})
    with urllib.request.urlopen(request, timeout=60) as response, temporary.open('xb') as stream:
        remaining = size + 1
        while remaining:
            data = response.read(min(1024 * 1024, remaining))
            if not data:
                break
            stream.write(data)
            remaining -= len(data)
    if temporary.stat().st_size != size or digest(temporary) != expected:
        raise ValueError('Downloaded release input differs: ' + url)
    temporary.rename(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('stock-audio', 'build'))
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        raise ValueError('Compose media preparation requires native macOS SDKs')
    work = args.work.resolve()
    if args.mode == 'stock-audio':
        if os.geteuid() != 0 or args.source is None:
            raise ValueError('Read-only stock audio extraction requires root and --source')
        from picasso_audio_route import STOCK_SHA256, INFO_SHA256, prepare_bytes, require_target
        from picasso_target import OS_SHA256
        import runpy
        require_target(ROOT)
        source = args.source.resolve()
        if digest(source) != OS_SHA256:
            raise ValueError('Stock audio OS differs from the pinned iPhone X input')
        output = work / 'stock-audio/VirtualAudio.plugin'
        mount = work / 'stock-audio/os-mount'
        preparation = runpy.run_path(str(ROOT / 'scripts/prepare-picasso-ram-data.py'))
        with preparation['mounted'](source, mount, readonly=True):
            plugin = mount / 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
            binary, info = (plugin / 'VirtualAudio').read_bytes(), (plugin / 'Info.plist').read_bytes()
            prepare_bytes(binary, info)
            output.mkdir(parents=True, exist_ok=False)
            (output / 'VirtualAudio').write_bytes(binary)
            (output / 'Info.plist').write_bytes(info)
        (output.parent / 'input-report.json').write_text(json.dumps(dict(
            os_sha256=OS_SHA256, binary_sha256=STOCK_SHA256, info_sha256=INFO_SHA256,
            source_readonly=True), indent=2) + '\n')
        return

    def xcrun(sdk, *arguments):
        return subprocess.check_output(['xcrun', '--sdk', sdk, *arguments], text=True).strip()

    sdk = Path(xcrun('iphoneos', '--show-sdk-path'))
    cc = xcrun('iphoneos', '-find', 'clang')
    prefix = Path(subprocess.check_output(['brew', '--prefix', 'lld'], text=True).strip())
    ld = prefix / 'bin/ld64.lld'
    nm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin/llvm-nm'
    for path in (sdk / 'SDKSettings.json', Path(cc), ld, nm):
        if not path.is_file():
            raise ValueError('Missing native SDK/tool input: ' + str(path))
    staged_headers = work / 'audio-headers/CoreAudio'
    staged_headers.mkdir(parents=True, exist_ok=True)
    # Retain the exact HAL interface used by the qualified iOS16 build.
    # New macOS headers may add unavailable/deprecated APIs to the iOS target.
    for name, (size, expected) in HEADERS.items():
        download(staged_headers / name, HEADER_URL + name, size, expected)
    os.environ.update(PICASSO_AUDIO_SDK=str(sdk), PICASSO_AUDIO_HEADERS=str(staged_headers.parent),
                      PICASSO_AUDIO_CC=cc, PICASSO_AUDIO_LD=str(ld),
                      PICASSO_AUDIO_VIRTUAL_AUDIO=str(work / 'stock-audio/VirtualAudio.plugin'),
                      PICASSO_GL_SDK=str(sdk), PICASSO_GL_CC=cc, PICASSO_GL_LD=str(ld), PICASSO_GL_NM=str(nm),
                      PICASSO_UI_SDK=str(sdk), PICASSO_UI_CC=cc, PICASSO_UI_LD=str(ld))
    from picasso_media_payload import GPU_ADDON, GPU_SHA, GPU_TRUST_SHA
    for name, size, expected in ((GPU_ADDON, 16114464, GPU_SHA), ('gpu-trust.bundle', 67960, GPU_TRUST_SHA)):
        download(work / 'gpu72' / name, GPU_URL + name, size, expected)
    for script, arguments in (
        ('build-picasso-audio-hal.py', ['--output', str(work / 'audio-hal')]),
        ('picasso_audio_payload.py', ['prepare', '--work', str(work)]),
        ('picasso_nativegl.py', ['--work', str(work)]),
        ('picasso_media_payload.py', ['prepare', '--work', str(work)]),
    ):
        subprocess.run([sys.executable, '-B', str(ROOT / 'scripts' / script), *arguments], check=True)


if __name__ == '__main__':
    main()
