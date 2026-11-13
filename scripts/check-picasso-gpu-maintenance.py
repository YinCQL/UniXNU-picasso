#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise production GPU maintenance waits and fallback-channel activation."""
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def extract(s, signature):
    a=s.index(signature); b=s.index('{',a); depth=1; c=b+1
    while depth:
        depth+=(s[c]=='{')-(s[c]=='}'); c+=1
    return s[a:c]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cxx',default=shutil.which('clang++'))
p.add_argument('--source',type=Path,default=ROOT/'drivers/NativeAdreno/NativeAdrenoPublish.cpp')
a=p.parse_args(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=False)
s=a.source.read_text()
if 'static void nativeAdrenoHangcheckWait()' in s:
    wait=extract(s,'static void nativeAdrenoHangcheckWait()')
else:
    # Run the old production wait body against the same no-diagnostic requirement.
    start=s.index('        for(unsigned tick=0;tick<500;++tick)')
    end=s.index('        native_adreno_hangcheck[0]',start)
    wait='static void nativeAdrenoHangcheckWait() {\n'+s[start:end]+'}\n'
(out/'wait.inc').write_text(wait)
runs=[]
def run(name, commands):
    for stage,cmd in commands:
        q=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
        (out/(name+'-'+stage+'.log')).write_text(q.stdout+q.stderr)
        runs.append(dict(name=name+'-'+stage,command=cmd,exit=q.returncode))
        if q.returncode:
            (out/'results.json').write_text(json.dumps(dict(passed=False,runs=runs),indent=2)+'\n')
            raise SystemExit(q.stdout+q.stderr)
for name in ['wait','mailbox']:
    if name=='mailbox':
        mount=extract(s,'bool AGXDeviceUserClient::mount(')
        assert 'kernel_thread_start(nativeAdrenoMailboxPoller' not in mount
        dispatch=extract(s,'IOReturn AGXDeviceUserClient::clientMemoryForType(')
        assert 'if(type==NativeAdrenoMailboxType)return mailboxMemory(options,memory);' in dispatch
        (out/'mailbox.inc').write_text(extract(s,'IOReturn AGXDeviceUserClient::mailboxMemory('))
    for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
        exe=out/(name+'-'+variant)
        cmd=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread',*flags,
             '-I',str(out),str(ROOT/('scripts/tests/picasso-gpu-'+name+'-host.cpp')),'-o',str(exe)]
        run(name+'-'+variant,[('compile',cmd),('run',[str(exe)])])
hang=extract(s,'static void nativeAdrenoHangcheck(void *,wait_result_t)')
assert hang.count('nativeAdrenoHangcheckWait();')==1
assert hang.index('nativeAdrenoHangcheckWait();') < hang.index('g.submit->hangCheck(&hung)')
assert 'g.submit->recover(g.restart,g.restart_cookie,&g.clock,&result)' in hang
inputs=[a.source,Path(__file__),ROOT/'scripts/tests/picasso-gpu-wait-host.cpp',ROOT/'scripts/tests/picasso-gpu-mailbox-host.cpp']
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,
    inputs={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}),indent=2)+'\n')
print('PASS GPU maintenance sleep budget, diagnostic fences and serialized on-demand mailbox activation')
