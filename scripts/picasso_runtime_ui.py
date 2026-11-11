#!/usr/bin/env python3
"""Build and qualify the signed runtime UI library without changing its Data seed."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
from picasso_ui_trust import library_identity, make_bundle
ROOT = Path(__file__).resolve().parents[1]
FILE = 'ui/libpicasso-sb.dylib'
SOURCES = ['package/picasso-springboard/' + n for n in
           ['picasso-ui-runtime.m', 'picasso-input-cache.m', 'picasso-springboard-probe.m',
            'picasso-statusbar-metrics.m', 'picasso-astc-guard.m', 'picasso-astc-guard-core.h',
            'picasso-pixel-sample.h', 'picasso-wallpaper-probe.m']]
SOURCES += ['scripts/picasso_runtime_ui.py']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def build(work):
    out = work / 'runtime-ui'; out.mkdir(parents=True, exist_ok=True)
    sdk = Path(os.environ.get('PICASSO_UI_SDK', '/home/y/toolchains/audio-sdks/iPhoneOS16.5.sdk'))
    cc = os.environ.get('PICASSO_UI_CC', '/usr/bin/clang-18')
    ld = os.environ.get('PICASSO_UI_LD', '/usr/bin/ld64.lld-18')
    commands = []
    def run(name, cmd):
        commands.append(cmd)
        p = subprocess.run(cmd, capture_output=True, text=True)
        (out / (name + '.log')).write_text(p.stdout + p.stderr)
        if p.returncode: raise RuntimeError(p.stdout + p.stderr)
        return p.stdout
    src = ROOT / SOURCES[0]
    flags = [cc, '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk), '-fobjc-arc',
             '-fblocks', '-O2', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror']
    run('compile', flags + ['-c', str(src), '-o', str(out / 'runtime.o')])
    deps = run('dependencies', flags + ['-M', '-MT', 'runtime', str(src)])
    inputs = {n: sha(Path(n)) for n in [cc, ld]}
    for n in deps.replace('\\\n', ' ').split()[1:]:
        p = Path(n)
        if p.is_file() and p.is_relative_to(sdk): inputs[str(p)] = sha(p)
    for n in ['Foundation', 'UIKit', 'Metal', 'CoreGraphics']:
        p = sdk / ('System/Library/Frameworks/' + n + '.framework/' + n + '.tbd')
        inputs[str(p)] = sha(p)
    for p in (sdk / 'usr/lib').glob('*.tbd'): inputs[str(p)] = sha(p)
    lib = out / 'libpicasso-sb.dylib'
    run('link', [ld, '-arch', 'arm64', '-platform_version', 'ios', '16.0', '16.5', '-dylib',
                '-install_name', '/usr/lib/libpicasso-sb.dylib', '-syslibroot', str(sdk),
                '-lSystem', '-lobjc', '-framework', 'Foundation', '-framework', 'UIKit',
                '-framework', 'Metal', '-framework', 'CoreGraphics', str(out / 'runtime.o'), '-o', str(lib), '-adhoc_codesign'])
    identity = library_identity(lib.read_bytes())
    if identity['code_page_bytes'] != 4096 or b'PICASSO_INPUT_CACHE' not in lib.read_bytes():
        raise ValueError('Runtime UI code signature or marker missing')
    report = dict(passed=True, runtime_qualified=False, identity=identity, commands=commands,
                  sources={n: sha(ROOT/n) for n in SOURCES}, inputs=inputs)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
    return report
def verify(work):
    out = work / 'runtime-ui'
    r = json.loads((out/'build-report.json').read_text())
    if set(r['sources']) != set(SOURCES) or not r['passed']: raise ValueError('Runtime UI source set differs')
    for n,h in r['sources'].items():
        if sha(ROOT/n) != h: raise ValueError('Stale runtime UI source: '+n)
    for n,h in r['inputs'].items():
        if sha(Path(n)) != h: raise ValueError('Changed runtime UI input: '+n)
    lib = (out/'libpicasso-sb.dylib').read_bytes()
    if library_identity(lib) != r['identity'] or r['identity']['code_page_bytes'] != 4096:
        raise ValueError('Changed signed runtime UI')
    return lib,r
def replace_module(work, modules):
    from picasso_media_payload import bundle, modules as split
    lib,report = verify(work)
    updated,ui = make_bundle(bundle(modules[:2]), lib)
    return modules[:2] + [split(updated)[2]] + modules[3:], lib, dict(build=report, trust=ui)
if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    a=p.parse_args();print(json.dumps(build(a.work.resolve())['identity']))
