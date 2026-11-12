#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production side-button wrapper with real Objective-C dispatch."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
source=ROOT/'package/picasso-springboard/picasso-power-button.m'
s=source.read_text();s=s[:s.index('static void picasso_power_image_added')]
start=s.index('static void picasso_power_note');end=s.index('__attribute__((noinline))')
s=s[:start]+s[end:]
# GNU dispatch has no objc_msgSend. These two adapters do normal method lookup.
s=s.replace('((id (*)(id, SEL))objc_msgSend)','host_get').replace('((void (*)(id, SEL))objc_msgSend)','host_present')
# The host functions have unrelated link addresses. Keep all production guard
# comparisons, substituting the three fixture IMP offsets for the target offsets.
s=s.replace('0x3a3a48','(uintptr_t)original').replace('0x8ef4','(uintptr_t)get_workspace').replace('0xdb294','(uintptr_t)show_workspace')
(out/'power-wrapper.inc').write_text(s)
fixture=ROOT/'scripts/tests/picasso-power-button-objc.m';runs=[]
for san in [False,True]:
    exe=out/('power-san' if san else 'power')
    cmd=['clang-18','-x','objective-c','-I/usr/lib/gcc/x86_64-linux-gnu/13/include','-I',str(out),'-O1','-fno-omit-frame-pointer','-fno-optimize-sibling-calls','-Wall','-Wextra','-Werror','-Wno-frame-address',str(fixture),'-lobjc','-o',str(exe)]
    if san:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    for name,command in [('compile',cmd),('run',[str(exe)]),('return-abi',[str(exe),'return']),('args-abi',[str(exe),'args'])]:
        q=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        (out/(exe.name+'-'+name+'.log')).write_text(q.stdout+q.stderr)
        assert q.returncode==0,q.stdout+q.stderr
        runs.append(dict(sanitizer=san,stage=name,passed=True,output=q.stdout))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,source_sha256=sha(source),inputs={str(p.relative_to(ROOT)):sha(p) for p in [source,fixture,Path(__file__)]},runs=runs,limitations='GNU Objective-C with host IMP offsets and dispatch adapters; actual return address builtin. Stock UUID/hash guards and ARM64 ABI require SDK/stock-image verification; physical button recognition and overlay display require device verification.'),indent=2)+'\n')
print('PASS power wrapper normal/ASan/UBSan, caller/thread/fallback and ABI guards')
