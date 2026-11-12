#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the production battery publisher against an iOS percentage policy model."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--source',type=Path,default=ROOT/'package/PicassoAccelerator.cpp')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
source=a.source.read_text()
start=source.index('void PicassoPowerSource::publish(')
end=source.index('\nbool PicassoPowerSource::start(',start)
publish=source[start:end].replace('PicassoPowerSource::','Publisher::')
start=source.index('            flag("PicassoBatteryDataStale",true);')
end=source.index('            updateStatus();',start)+len('            updateStatus();')
stale='void Publisher::stale(){\n'+source[start:end]+'\n}\n'
(out/'publisher.inc').write_text(publish+'\n'+stale)
runs=[]
for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
    exe=out/variant
    cmd=['clang++-18','-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,
         '-I',str(ROOT/'package'),'-I',str(out),str(ROOT/'scripts/tests/picasso-battery-publish-host.cpp'),'-o',str(exe)]
    for kind,command in [('compile',cmd),('run',[str(exe)])]:
        q=subprocess.run(command,capture_output=True,text=True,timeout=60)
        (out/(variant+'-'+kind+'.log')).write_text(q.stdout+q.stderr)
        if q.returncode:raise SystemExit(q.stdout+q.stderr)
        runs.append(dict(name=variant+'-'+kind,passed=True,command=command,output=q.stdout))
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,
    scope=__doc__,source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),runs=runs),indent=2)+'\n')
print('PASS actual battery publication, stale cleanup and modeled iOS percentage policy; host only')
