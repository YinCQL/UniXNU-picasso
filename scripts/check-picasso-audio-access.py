#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check scoped audio access, addon visibility, and actual DSP-idle wait helpers."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--cxx',default=shutil.which('clang++'));a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);runs=[]
    patch=ROOT/'patches/picasso-audio-access-xnu.patch'
    added='\n'.join(line[1:] for line in patch.read_text().splitlines() if line.startswith(('+',' ')) and not line.startswith('+++'))+'\n'
    def extract(text,name):
        start=text.index(name+'(');start=text.rfind('static ',0,start);end=text.index('\n}',start)+2;return text[start:end]+'\n'
    for target,name in [('access','picasso_audio_open_grant'),('mount','picasso_audio_addon_mount')]:
        (out/(target+'.inc')).write_text(extract(added,name))
    (out/'idle.inc').write_text(extract((ROOT/'hal/PicassoAudioPlugIn.c').read_text(),'waitIdle'))
    for sanitized in [False,True]:
        suffix='-asan' if sanitized else '';flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else []
        exe=out/('access'+suffix)
        commands=[('compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,'-I',str(out),'-I',str(ROOT/'package'),str(ROOT/'scripts/tests/picasso-audio-access-host.cpp'),'-o',str(exe)]),('run'+suffix,[str(exe)])]
        for name,cmd in commands:
            run=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(out/(name+'.log')).write_text(run.stdout+run.stderr)
            if run.returncode:raise RuntimeError(run.stdout+run.stderr)
            runs.append(dict(name=name,passed=True))
    paths=['patches/picasso-audio-access-xnu.patch','hal/PicassoAudioPlugIn.c','scripts/tests/picasso-audio-access-host.cpp','scripts/check-picasso-audio-access.py']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in paths}),indent=2)+'\n')
    print('PASS signed audio access / mount visibility / bounded idle wait (normal and ASan/UBSan)')
if __name__=='__main__':main()
