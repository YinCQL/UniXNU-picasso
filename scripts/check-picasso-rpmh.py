#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the shared RSC engine and exact GPU/DSP owner body on host and ARM64."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cxx',default=shutil.which('clang++'))
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    runs = []

    def run(name,command):
        r = subprocess.run(command,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf8',newline='\n')
        if r.returncode:
            raise RuntimeError(r.stdout+r.stderr)
        runs.append(dict(name=name,passed=True,output=r.stdout))

    cp = (ROOT/'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp').read_text()
    owner = cp[cp.index('constexpr uint64_t kAppsRscDrv2Base'):cp.index('/* wf119:')]
    exports = cp[cp.index('extern "C" int picasso_rpmh_active_write'):cp.index('// PIL calls and the reserved metadata')]
    body = 'namespace {\n'+owner+'\n}\n'+exports
    assert 'if (rpmhVoteCryptoPath() != 7) return false;' in cp
    (out/'PicassoRpmh-owner-body.inc').write_text(body,encoding='utf8',newline='\n')
    includes = [x for d in (out,ROOT/'drivers/NativeAdrenoPicasso',ROOT/'scripts/tests') for x in ('-I',str(d))]
    for sanitized in (False,True):
        suffix = '-asan' if sanitized else ''
        flags = (['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else [])
        for name in ('rpmh','rpmh-device'):
            exe = out/(name+suffix+('.exe' if os.name=='nt' else ''))
            run(name+'-compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
                str(ROOT/f'scripts/tests/picasso-{name}-host.cpp'),'-o',str(exe)])
            run(name+suffix,[str(exe)])
    arm = out/'rpmh-arm.cpp'
    arm.write_text('#include "picasso-rpmh-device-api.h"\n'+body+
        '\nextern "C" unsigned test_gpu_votes() { return rpmhVoteCryptoPath(); }\n',encoding='utf8',newline='\n')
    run('rpmh-arm64',[a.cxx,'-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding','-fno-exceptions',
        '-fno-rtti','-fno-threadsafe-statics','-mgeneral-regs-only','-Wall','-Wextra','-Werror',*includes,
        '-c',str(arm),'-o',str(out/'rpmh.o')])
    inputs = ['drivers/NativeAdrenoPicasso/Adreno620Cp.cpp','drivers/NativeAdrenoPicasso/PicassoRpmh.h',
        'scripts/check-picasso-rpmh.py','scripts/tests/picasso-rpmh-host.cpp','scripts/tests/picasso-rpmh-device-host.cpp',
        'scripts/tests/picasso-rpmh-device-api.h','scripts/build-picasso-display.py','scripts/build-arm-driver.py','.github/workflows/host-checks.yml']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,
        dsp_powered=False,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}),indent=2)+'\n',encoding='utf8',newline='\n')
    print('PASS shared RPMh command engine, production GPU/ADSP owner and ARM64 API model')


if __name__=='__main__':
    main()
