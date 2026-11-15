#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify default-off diagnostic capture and loss-detecting PCM log export."""
import argparse
import hashlib
import importlib.util
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
    exported=''
    for sanitizer in (False,True):
        suffix='-asan' if sanitizer else '';exe=out/('mic-service'+suffix)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitizer else []
        run('compile'+suffix,[args.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
            str(ROOT/'scripts/tests/picasso-mic-service-host.cpp'),'-o',str(exe)])
        for mode in range(4):
            result=run(str(mode)+suffix,[str(exe),str(mode)])
            if mode==2:exported=result
    spec=importlib.util.spec_from_file_location('decode_mic',ROOT/'scripts/decode-picasso-mic.py')
    decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    assert decoder.decode(exported)==[bytes(range(130))]
    first=next(line for line in exported.splitlines() if 'pcm offset=0 ' in line)
    malformed=[exported.replace(first,first+'\n'+first),exported.replace('offset=64','offset=62'),exported.replace('bytes=64','bytes=63',1),
               exported.replace('offset=0 bytes=64','offset=2 bytes=64',1),
               re.sub(r'fnv1a=[0-9a-f]+','fnv1a=00000000',exported),
               '\n'.join(line for line in exported.splitlines() if 'pcm-end' not in line)]
    for text in malformed:
        try:decoder.decode(text)
        except ValueError:pass
        else:raise AssertionError('Accepted damaged capture')
    run('decode-wave',['python3','-B',str(ROOT/'scripts/decode-picasso-mic.py'),str(out/'2-asan.log'),'--output',str(out/'capture.wav')])
    runs.append({'name':'decode-loss-and-corruption','passed':True})
    names=['package/PicassoMicService.cpp','package/PicassoMicService.h','package/PicassoCaptureDma.h',
           'scripts/decode-picasso-mic.py','scripts/tests/picasso-mic-service-host.cpp','scripts/check-picasso-mic-service.py']
    (out/'results.json').write_text(json.dumps({'passed':True,'hardware_tested':False,'runs':runs,
        'inputs':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}},indent=2)+'\n')
    print('PASS capture diagnostic service and checked WAV reconstruction')
if __name__=='__main__':main()
