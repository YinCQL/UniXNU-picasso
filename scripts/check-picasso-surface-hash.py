#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Validate full-byte surface hashing and report host timing without device claims."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
fixture=ROOT/'scripts/tests/picasso-surface-hash-host.c';header=ROOT/'drivers/NativeGL/eagl/surface_hash.h'
runs=[]
for name,flags in [('normal',[]),('asan',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
    exe=out/name
    for step,cmd in [('compile',['clang-18','-O2','-Wall','-Wextra','-Werror',*flags,'-I',str(header.parent),str(fixture),'-o',str(exe)]),('run',[str(exe)])]:
        q=subprocess.run(cmd,capture_output=True,text=True,timeout=90,env=dict(os.environ))
        (out/(name+'-'+step+'.log')).write_text(q.stdout+q.stderr)
        assert q.returncode==0,q.stdout+q.stderr
        runs.append(dict(name=name+'-'+step,passed=True,output=q.stdout))
sha=lambda n:hashlib.sha256(n.read_bytes()).hexdigest()
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,inputs={str(n.relative_to(ROOT)):sha(n) for n in [fixture,header,Path(__file__)]},runs=runs),indent=2)+'\n')
print(runs[1]['output'])
