#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production capture DMA adapter with isolated MMIO and allocations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cxx',default=shutil.which('clang++'))
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    body=(ROOT/'package/PicassoRpcDma.cpp').read_text()
    body=re.sub(r'^#include <[^>]+>[^\n]*\n','',body,flags=re.M)
    body,count=re.subn(r'__asm__ volatile\([^;]+;','dmaBarrier();',body)
    assert count==3
    (out/'PicassoRpcDma-body.inc').write_text(body)
    includes=[x for d in (out,ROOT/'package',ROOT/'drivers/NativeWlan') for x in ('-I',str(d))]
    runs=[]
    def run(name,args):
        r=subprocess.run(args,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        runs.append({'name':name,'passed':True})
    for sanitized in (False,True):
        suffix='-asan' if sanitized else ''
        exe=out/('capture-dma'+suffix)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else []
        run('compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
            str(ROOT/'scripts/tests/picasso-capture-dma-host.cpp'),str(ROOT/'package/PicassoRpcFs.cpp'),'-o',str(exe)])
        for case in range(8):run(str(case)+suffix,[str(exe),str(case)])
    inputs=['package/PicassoCapture.h','package/PicassoCaptureDma.h','package/PicassoPcm.h',
            'package/PicassoRpcDma.h','package/PicassoRpcDma.cpp','scripts/tests/picasso-capture-dma-host.cpp',
            'scripts/check-picasso-capture-dma.py']
    (out/'results.json').write_text(json.dumps({'passed':True,'hardware_tested':False,'runs':runs,
        'inputs':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}},indent=2)+'\n')
    print('PASS production capture DMA allocator and completion adapter')
if __name__=='__main__':main()
