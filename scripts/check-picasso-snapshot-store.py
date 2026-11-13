#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the production snapshot file transaction and unprotected-Data gates."""
from pathlib import Path
import argparse,subprocess,json,hashlib
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();O=a.output.resolve();O.mkdir(parents=True,exist_ok=False)
sources=[R/'package/picasso-springboard/picasso-snapshot-store-core.h',R/'scripts/tests/picasso-snapshot-store-host.c']
runs=[]
for name,flags in [('normal',['-O2']),('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
    exe=O/name
    for step,cmd in [('compile',['clang-18','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(sources[0].parent),str(sources[1]),'-o',str(exe)]),('run',[str(exe)])]:
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=60);(O/(name+'-'+step+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise SystemExit(r.stdout+r.stderr)
        runs.append(dict(name=name+'-'+step,passed=True,command=cmd,output=r.stdout))
    print(name,'passed',flush=True)
(O/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,sources={str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},runs=runs),indent=2)+'\n')
