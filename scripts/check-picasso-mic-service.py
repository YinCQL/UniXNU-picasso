#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify on-demand input service, independent ownership and timestamped PCM."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cxx',default=shutil.which('clang++'));args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    body=re.sub(r'^#include <[^>]+>[^\n]*\n','',(ROOT/'package/PicassoMicService.cpp').read_text(),flags=re.M)
    (out/'PicassoMicService-body.inc').write_text(body)
    includes=[x for d in (out,ROOT/'package',ROOT/'drivers/NativeWlan') for x in ('-I',str(d))]
    runs=[]
    def run(name,command):
        result=subprocess.run(command,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        runs.append({'name':name,'passed':True});return result.stdout
    for sanitizer in (False,True):
        suffix='-asan' if sanitizer else '';exe=out/('mic-service'+suffix)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitizer else []
        run('compile'+suffix,[args.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
            str(ROOT/'scripts/tests/picasso-mic-service-host.cpp'),'-o',str(exe)])
        run('service'+suffix,[str(exe)])
    for name in ['package/PicassoMicService.cpp','package/PicassoSmp2pDevice.cpp']:
        source=(ROOT/name).read_text()
        assert 'picasso-mic-test' not in source and 'pcm offset=' not in source
    names=['package/PicassoSmp2pDevice.cpp','package/PicassoMicService.cpp','package/PicassoMicService.h','package/PicassoCaptureDma.h',
           'package/PicassoAudioInput.h','package/PicassoAudioAbi.h','scripts/tests/picasso-mic-service-host.cpp','scripts/check-picasso-mic-service.py']
    (out/'results.json').write_text(json.dumps({'passed':True,'hardware_tested':False,'runs':runs,
        'inputs':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}},indent=2)+'\n')
    print('PASS on-demand capture service and bounded timestamped input')
if __name__=='__main__':main()
