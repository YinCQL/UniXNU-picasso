#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Board power sequencing and bounded battery observations over host doubles."""
from pathlib import Path
import argparse,hashlib,json,subprocess,re
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
s=(ROOT/'package/PicassoPower.cpp').read_text();start=s.index('static int halt(unsigned type)');end=s.index('\n}',start)+2
(out/'power-dispatch.inc').write_text(s[start:end]+'\n')
assert 'PE_halt_restart=halt;' in s and 'next->previous=PE_halt_restart;' in s
battery=(ROOT/'package/PicassoBattery.cpp').read_text()
assert 'picasso_spmi_write8' not in battery and 'soc_live=0' in battery
assert 'picasso_power_start();' in (ROOT/'package/PicassoDisplay.cpp').read_text()
smc=s[s.index('struct Reply'):s.index('struct Backend')]
smc=re.sub(r'register uint64_t (x[0-7]) __asm__\("x[0-7]"\)',r'uint64_t \1',smc)
smc=re.sub(r'__asm__ volatile\("smc #0".*?\);','fake_smc(x0,x1,x2,x3,x4,x5,x6,x7);',smc,flags=re.S)
(out/'power-smc.inc').write_text(smc)
runs=[]
def run(name,cmd):
 q=subprocess.run(cmd,capture_output=True,text=True,timeout=90);(out/(name+'.log')).write_text(q.stdout+q.stderr)
 if q.returncode:raise RuntimeError(q.stdout+q.stderr)
 runs.append(dict(name=name,command=list(map(str,cmd)),passed=True,output=q.stdout));return q
for kind in ['power','battery','smc']:
 for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  name=kind+'-'+variant;exe=out/name
  run(name+'-compile',['clang++-18','-std=c++17','-Wall','-Wextra','-Werror','-O1',*flags,'-I',str(ROOT/'package'),'-I',str(out),str(ROOT/'scripts/tests'/('picasso-'+kind+'-host.cpp')),'-o',str(exe)])
  run(name+'-run',[str(exe)])
for name in ['PicassoPower','PicassoBattery']:
 body=(ROOT/'package'/(name+'.cpp')).read_text();body=re.sub(r'^#include <[^>]+>[^\n]*\n','',body,flags=re.M)
 (out/(name+'.cpp')).write_text('#include "picasso-input-kernel-api.h"\nextern "C" int (*PE_halt_restart)(unsigned);\nenum {kPEHaltCPU=0};\n'+body)
 run(name+'-arm64',['clang++-18','-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-fno-c++-static-destructors','-mgeneral-regs-only','-Wall','-Wextra','-Werror','-I',str(ROOT/'scripts/tests'),'-I',str(ROOT/'package'),'-c',str(out/(name+'.cpp')),'-o',str(out/(name+'.o'))])
inputs=['package/PicassoPower.cpp','package/PicassoPowerCore.h','package/PicassoBattery.cpp','package/PicassoBatteryCore.h','package/PicassoSpmi.cpp','package/PicassoSpmiCore.h','package/PicassoSpmi.h','package/PicassoDisplay.cpp','scripts/build-picasso-display.py','scripts/check-picasso-display-artifacts.py','scripts/check-picasso-power.py','scripts/tests/picasso-power-host.cpp','scripts/tests/picasso-battery-host.cpp','scripts/tests/picasso-smc-host.cpp']
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,scope=__doc__,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}),indent=2)+'\n')
print('PASS power sequencing, callback dispatch, QG observations and ARM64 compilation; host doubles only')
