#!/usr/bin/env python3
"""Exercise the replayed SpringBoard self-sampler with host API models."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = (args.xnu / 'osfmk/arm64/sleh.c').read_text()
    start = source.index('static void\npicasso_ui_self_sample(')
    end = source.index('\n}\n', start) + 3
    (args.output / 'ui-self-sample.inc').write_text(source[start:end])
    assert source.count('picasso_ui_self_sample(state, thread, trap_no);') == 1
    remote = (args.xnu / 'osfmk/ipc/mach_debug.c').read_text()
    assert 'cache_slide=%x coherent=0' in remote
    runs = []
    for sanitized in (False, True):
        name = 'self-sample' + ('-sanitized' if sanitized else '')
        exe = args.output / name
        command = [shutil.which('clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                   '-I', str(args.output), str(ROOT / 'scripts/tests/picasso-ui-self-sample-host.c'), '-o', str(exe)]
        if sanitized:
            command += ['-fsanitize=address,undefined']
        for stage, cmd in [('compile', command), ('run', [str(exe)])]:
            result = subprocess.run(cmd, text=True, capture_output=True)
            (args.output / (name + '-' + stage + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        runs.append({'name': name, 'passed': True})
    (args.output / 'results.json').write_text(json.dumps({'passed': True, 'scope': __doc__, 'runs': runs,
        'source_sha256': hashlib.sha256(source.encode()).hexdigest()}, indent=2)+'\n')
    print('PASS replayed SpringBoard self-sampler: normal and ASan/UBSan')

if __name__ == '__main__':
    main()
