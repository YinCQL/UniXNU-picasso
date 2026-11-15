#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read-only WCD supply qualification and production wrapper over host doubles."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
inputs=['package/PicassoMicPower.h','package/PicassoMicPower.cpp','package/PicassoSpmi.h',
        'scripts/tests/picasso-mic-power-host.cpp','scripts/tests/picasso-mic-power-backend-host.cpp',
        'scripts/check-picasso-mic-power.py']
result=dict(passed=False,hardware_tested=False,kernel_sdk_compiled=False,
            scope=__doc__,runs=[],inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs})
def run(name,command):
    command=list(map(str,command))
    q=subprocess.run(command,capture_output=True,text=True,timeout=90)
    (out/(name+'.log')).write_text(q.stdout+q.stderr)
    result['runs'].append(dict(name=name,command=command,exit_code=q.returncode,passed=q.returncode==0))
    if q.returncode:raise RuntimeError(name+'\n'+q.stdout+q.stderr)
try:
    source=(ROOT/'package/PicassoMicPower.cpp').read_text()
    source,count=re.subn(r'^#include <IOKit/[^>]+>\n','',source,flags=re.M)
    assert count==4
    source,count=re.subn(r'__asm__ volatile\("dsb sy" ::: "memory"\);',
                        '__atomic_thread_fence(__ATOMIC_SEQ_CST);',source)
    assert count==1
    (out/'picasso-mic-power-backend.inc').write_text(source)
    result['host_adaptation']='Remove four IOKit includes, provide test API doubles, replace one ARM64 DSB with a host atomic fence; all production control flow is unchanged.'
    result['host_adapted_source_sha256']=hashlib.sha256(source.encode()).hexdigest()
    for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
        for kind in ['core','backend']:
            name='mic-power-'+kind+'-'+variant;exe=out/name
            fixture='picasso-mic-power-'+('backend-host.cpp' if kind=='backend' else 'host.cpp')
            run(name+'-compile',['clang++-18','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-pthread',
                *flags,'-I',ROOT/'package','-I',out,ROOT/'scripts/tests'/fixture,'-o',exe])
            if kind=='core':run(name+'-run',[exe])
            else:
                for scenario in ['success','mapping','busfault','qualification']:
                    run(name+'-'+scenario,[exe,scenario])
                for at in [1,8,30,52]:run(name+'-timeout-'+str(at),[exe,'timeout',str(at)])
    result['passed']=True
finally:
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS read-only microphone rails, all read faults, unstable snapshots, zero mapping addresses, cleanup and timeout poisoning; host doubles only')
