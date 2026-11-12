#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual root-domain shutdown hook; host callbacks are emulated."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root-domain',type=Path,required=True)
p.add_argument('--baseline',type=Path)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('extractor',ROOT/'scripts/check-picasso-buttons.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
runs=[]
for version,path in [('current',a.root_domain),('baseline',a.baseline)]:
 if path is None:continue
 source=path.read_text();body=m.extract(source,'static void\nIOPMRootDomainWillShutdown(')
 hooked='if (picasso_backlight_shutdown) picasso_backlight_shutdown();' in body
 assert hooked==(version=='current')
 # Only the irreversible commit path invokes the board callback. The ordinary
 # early countdown property is reversible and must not blank the backlight.
 if hooked:
  assert source.count('if (picasso_backlight_shutdown) picasso_backlight_shutdown();')==1
  assert body.index('OSCompareAndSwap(0, 1, &gWillShutdown)')<body.index('picasso_backlight_shutdown();')<body.index('IOSleep(')
  setter=m.extract(source,'sysctl_willshutdown SYSCTL_HANDLER_ARGS')
  assert 'if (!gWillShutdown && (new_value == 1))' in setter and 'IOPMRootDomainWillShutdown();' in setter
 (out/('root-domain-'+version+'.inc')).write_text(body+'\n')
 for sanitized in [False,True]:
  name=version+('-sanitized' if sanitized else '-normal');exe=out/name
  cmd=['clang++-18','-std=c++17','-O1','-Wall','-Wextra','-Werror','-I',str(out),'-DROOT_BODY="root-domain-'+version+'.inc"','-DEXPECT_HOOK='+str(int(hooked)),str(ROOT/'scripts/tests/picasso-shutdown-host.cpp'),'-o',str(exe)]
  if sanitized:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
  for stage,command in [('compile',cmd),('run',[str(exe)])]:
   q=subprocess.run(command,capture_output=True,text=True,timeout=60);(out/(name+'-'+stage+'.log')).write_text(q.stdout+q.stderr)
   if q.returncode:raise RuntimeError(q.stdout+q.stderr)
   runs.append(dict(name=name+'-'+stage,passed=True,output=q.stdout,command=command))
inputs=['package/PicassoBacklight.cpp','package/PicassoBacklightCore.h','patches/picasso-shutdown-backlight-xnu.patch','scripts/check-picasso-shutdown.py','scripts/tests/picasso-shutdown-host.cpp','unixnu']
identity={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}
identity[str(a.root_domain.resolve())]=hashlib.sha256(a.root_domain.read_bytes()).hexdigest()
if a.baseline:identity[str(a.baseline.resolve())]=hashlib.sha256(a.baseline.read_bytes()).hexdigest()
assert "'shutdown-backlight'" in (ROOT/'unixnu').read_text()
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,scope=__doc__,runs=runs,inputs=identity),indent=2)+'\n')
print('PASS actual committed shutdown callback before pending wait, duplicate commit, absent backend and preserved OS sequence; host only')
