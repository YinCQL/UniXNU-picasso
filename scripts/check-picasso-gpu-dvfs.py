#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the production GPU governor and HFI vote with hostile MMIO peers."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def function(s,name):
 a=s.index(name);b=s.index('{',a);d=1;c=b+1
 while d:d+=(s[c]=='{')-(s[c]=='}');c+=1
 return s[a:c]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True);p.add_argument('--cxx',default='/usr/bin/clang++-18')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
pub=(ROOT/'drivers/NativeAdreno/NativeAdrenoPublish.cpp').read_text()
gmu=(ROOT/'drivers/NativeAdrenoPicasso/Adreno620Gmu.cpp').read_text()
(out/'gmu.inc').write_text('\n'.join(function(gmu,n) for n in ['extern "C" int native_adreno620_dvfs_read(','extern "C" int native_adreno620_dvfs_vote(']))
start=pub.index('static NativeAdrenoDvfsPolicy gDvfs;');end=pub.index('static void nativeAdrenoDvfsStart()',start)
(out/'publish.inc').write_text(pub[start:end])
# Integration: HFI is serialized with reset and dispatch; first startup probes
# precede governor activation; hangcheck retains its existing sleep budget.
run=function(pub,'static IOReturn runCore(AGXDeviceUserClient *connection')
assert run.index('lockTimed(lockWait);',run.index('NativeAdrenoSystemPages freed'))<run.index('nativeAdrenoDvfsWork();')<run.index('core_->dispatch(method,input')<run.index('unlockTimed(lockWait);',run.index('nativeAdrenoDvfsWork();'))
hang=function(pub,'static void nativeAdrenoHangcheck(void *,wait_result_t)')
assert hang.index('g.submit->recover(')<hang.index('gDvfs.invalidate()')<hang.index('IOLockUnlock(g.lock)')
publish=function(pub,'extern "C" IOReturn native_adreno_publish(')
assert publish.index('checkCpSwitch()')<publish.index('nativeAdrenoDvfsStart()')<publish.index('service->registerService()')
runs=[]
for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
 exe=out/variant
 command=[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread',*flags,'-I',str(out),'-I',str(ROOT/'drivers/NativeAdreno'),str(ROOT/'scripts/tests/picasso-gpu-dvfs-host.cpp'),'-o',str(exe)]
 for stage,cmd in [('compile',command),('run',[str(exe)])]:
  result=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
  (out/(variant+'-'+stage+'.log')).write_text(result.stdout+result.stderr)
  runs.append(dict(name=variant+'-'+stage,exit=result.returncode))
  if result.returncode:raise SystemExit(result.stdout+result.stderr)
names=['drivers/NativeAdreno/NativeAdrenoDvfs.h','drivers/NativeAdreno/NativeAdrenoPublish.cpp','drivers/NativeAdrenoPicasso/Adreno620Gmu.cpp','scripts/check-picasso-gpu-dvfs.py','scripts/tests/picasso-gpu-dvfs-host.cpp']
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}),indent=2)+'\n')
print('PASS DVFS bounds, hysteresis, XO wrap, reset, failure retention, wakeup and locked HFI')
