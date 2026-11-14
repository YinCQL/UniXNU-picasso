#!/usr/bin/env python3
"""Build the combined playback image while retaining the exact Data seed recipe."""
from pathlib import Path
import os
import runpy
import sys
import tomllib
import picasso_media_payload as media

ROOT = Path(__file__).resolve().parents[1]
def main():
    runpy.run_path(str(ROOT / 'scripts/check-picasso-target.py'))['check_board'](ROOT)
    work = ROOT / 'out/picasso'
    media.verify((work / 'media/media-trust.bundle').read_bytes(), work)
    base = tomllib.loads((ROOT / 'board.toml').read_text())['payload']['kernel_args']
    selected = media.kernel_args(base)
    tree = runpy.run_path(str(ROOT / 'unixnu'))['prepare']()
    os.environ.setdefault('UNIXNU_DEVICE', str(ROOT))
    os.environ.setdefault('UNIXNU_OUT', str(ROOT / 'out'))
    from picasso_target import BACKEND_IPSW
    os.environ.setdefault('UNIXNU_KERNELCACHE', str(ROOT / 'out/ipsw' / BACKEND_IPSW['build'] / 'kernelcache.decompressed'))
    sys.path.insert(0, str(tree / 'scripts'))
    from unixnu import payload, cli
    original = payload.check_args
    def compact(arguments):
        if arguments != base + ['picasso-audio-addon=1', *media.GPU_ARGS]:
            raise ValueError('Unexpected media image arguments')
        # Only final payload boot policy changes; recipe checks and the source
        # board still describe the byte-identical existing Data seed.
        arguments[:] = selected
        return original(arguments)
    payload.check_args = compact
    original_build = payload.build
    def build(ctx, kernel=None):
        spec = ctx.board['payload']
        if spec['size_mib'] != 60:
            raise ValueError('Changed base payload capacity')
        spec['size_mib'] = media.PAYLOAD_MIB
        try:
            return original_build(ctx, kernel)
        finally:
            spec['size_mib'] = 60
    payload.build = build
    extra = ['--owner-trust-cache', str(work/'media/media-trust.bundle'), '--gpu-addon', str(media.addon(work))]
    for arg in ['picasso-audio-addon=1', *media.GPU_ARGS]:
        extra += ['--kernel-arg', arg]
    cli.main(sys.argv[1:] + extra)

if __name__ == '__main__':
    main()
