#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production recovery and GPU_SET bodies with ordered MMIO peers."""
import argparse, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def function(text, signature):
    start=text.index(signature); body=text.index('{',start); depth=1; at=body+1
    while depth:
        depth += (text[at]=='{')-(text[at]=='}'); at+=1
    return text[start:at]
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--cxx',default=shutil.which('clang++'))
    args=ap.parse_args(); out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    cp=(ROOT/'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp').read_text()
    gmu=(ROOT/'drivers/NativeAdrenoPicasso/Adreno620Gmu.cpp').read_text()
    secure=function(cp,'static bool initializeSecureMode()')
    restart=function(cp,'extern "C" IOReturn native_adreno620_cp_restart(')
    hold=function(gmu,'extern "C" int native_adreno620_gmu_acquire_gpu_set(')
    release=function(gmu,'extern "C" void native_adreno620_gmu_release_gpu_set(')
    (out/'recovery.inc').write_text(secure+'\n'+restart+'\n')
    (out/'gpu-set.inc').write_text(hold+'\n'+release+'\n')
    # The same authentication gate is required before the first no-op and publication.
    start=function(cp,'extern "C" int native_adreno620_cp_start(')
    assert start.index('initializeSecureMode()') < start.index('submitNoop(')
    assert 'keeping secure mode' not in start
    runs=[]
    for sanitizer in (False,True):
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitizer else []
        for name in ('recovery','gpu-set'):
            suffix='-asan' if sanitizer else ''
            exe=out/(name+suffix)
            cmd=[args.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',*flags,
                 '-I',str(out),'-I',str(ROOT/'drivers/NativeAdreno'),
                 str(ROOT/('scripts/tests/picasso-gpu-'+name+'-host.cpp')),'-o',str(exe)]
            for label,command in [('compile',cmd),('run',[str(exe)])]:
                p=subprocess.run(command,capture_output=True,text=True,timeout=120)
                (out/(name+suffix+'-'+label+'.log')).write_text(p.stdout+p.stderr)
                if p.returncode:raise RuntimeError(p.stdout+p.stderr)
                runs.append(dict(name=name+suffix+'-'+label,passed=True))
    names=['drivers/NativeAdrenoPicasso/Adreno620Cp.cpp','drivers/NativeAdrenoPicasso/Adreno620Gmu.cpp',
           'drivers/NativeAdrenoPicasso/Adreno620Probe.h','drivers/NativeAdreno/NativeAdrenoA620HwInit.h',
           'scripts/check-picasso-gpu-recovery.py','scripts/tests/picasso-gpu-recovery-host.cpp',
           'scripts/tests/picasso-gpu-gpu-set-host.cpp']
    result=dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,runs=runs,
                inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names})
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS production GPU reset ordering, failure retention, secure startup and GPU_SET timeouts')
if __name__=='__main__':main()
