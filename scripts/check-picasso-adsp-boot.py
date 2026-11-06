#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise real ADSP startup/failure ordering and PAS resume on host and ARM64."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cxx',default=shutil.which('clang++'))
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    runs=[]
    def run(name,cmd):
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf8',newline='\n')
        if r.returncode:raise RuntimeError(name+'\n'+r.stdout+r.stderr)
        runs.append(dict(name=name,passed=True))
    body=re.sub(r'^#include <[^>]+>[^\n]*\n','',(ROOT/'package/PicassoAdspBoot.cpp').read_text(),flags=re.M)
    (out/'PicassoAdspBoot-body.inc').write_text(body,encoding='utf8',newline='\n')
    includes=[x for d in (out,ROOT/'package',ROOT/'drivers/NativeAdrenoPicasso',ROOT/'drivers/NativeWlan',ROOT/'scripts/tests') for x in ('-I',str(d))]
    cases=['normal','lease','firmware','plan','alloc1','alloc2','map1','map2','zero1','zero2','crypto',
        'aop','aop-uncertain','vote1','vote2','vote3','init','setup','load','auth','stale','pre-auth-peer',
        'timeout','fatal','interrupted','shutdown','off','close','memory-overlap','memory-missing','shutdown-vote','reacquire',
        'restart','missing','missing-entry','no-handover','initial-restart','restart-no-entry','lost-ready','long-ready']
    for sanitized in (False,True):
        suffix='-asan' if sanitized else ''
        flags=(['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else [])
        for fixture in ('adsp-boot','pas'):
            exe=out/(fixture+suffix+('.exe' if os.name=='nt' else ''))
            run(fixture+'-compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,str(ROOT/f'scripts/tests/picasso-{fixture}-host.cpp'),'-o',str(exe)])
            for case in cases if fixture=='adsp-boot' else ['normal']:run(fixture+'-'+case+suffix,[str(exe),case])
    api='#include "picasso-wlan-kernel-api.h"\nextern "C" {void IOLog(const char *,...) __attribute__((format(printf,1,2)));void IOSleep(unsigned);uint64_t mach_absolute_time();void clock_interval_to_deadline(unsigned,unsigned,uint64_t *);}\nenum {kSecondScale=1};\n'
    boot_api='struct boot_args {uint64_t physBase,memSize;}; extern struct PeState {void *bootArgs;} PE_state;\n'
    (out/'boot-arm.cpp').write_text(api+boot_api+body,encoding='utf8',newline='\n')
    # Compile the production SMC register wrapper and shared lease exports too.
    spec=importlib.util.spec_from_file_location('extract',ROOT/'scripts/check-picasso-gop-handoff.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    cp=(ROOT/'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp').read_text()
    scm=m.extract(cp,'ScmReply scmSipCall(')
    exports=cp[cp.index('// PIL calls and the reserved metadata'):cp.index('extern "C" int native_adreno620_cp_start')]
    probe=m.extract(cp,'void scmProbe()')
    lease=exports[:exports.index('static PicassoPasReply adspScm')]
    gpu=m.extract(cp,'    auto pasCall = ')
    # Drop only host-incompatible assembly; compile the unchanged GPU lambda
    # below for ARM64 as well. Protocol calls, loops and x5/x6 arguments remain.
    gpu_host=re.sub(r'__asm__ volatile\(.*?\);','',gpu,flags=re.S)
    gpu_host=gpu_host.replace('uint64_t pmr = 0, daif = 0;', 'uint64_t pmr = 0, daif = 0; (void)pmr; (void)daif;')
    for name,body_part in [('lease',lease),('probe',probe),('gpu',gpu_host+';')]:
        (out/f'PicassoPas-{name}.inc').write_text(body_part,encoding='utf8',newline='\n')
    for sanitized in (False,True):
        suffix='-asan' if sanitized else ''
        flags=(['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else [])
        exe=out/('pas-owner'+suffix+('.exe' if os.name=='nt' else ''))
        run('pas-owner-compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,str(ROOT/'scripts/tests/picasso-pas-owner-host.cpp'),'-o',str(exe)])
        run('pas-owner'+suffix,[str(exe)])
    gpu_arm='\nScmReply gpuResumeModel(){struct {bool uncertain=false;} lease;auto dumpGic=[](){};\n'+gpu+';\nreturn pasCall(0x42000201,0x82,13,0x8bc18000,0,true);}\n'
    (out/'pas-arm.cpp').write_text('#include "PicassoPas.h"\nusing ScmReply=PicassoPasReply;\nextern "C" int printf(const char *,...) __attribute__((format(printf,1,2)));\nvoid IOSleep(unsigned);\n'+scm+'\n'+exports+'\n'+probe+gpu_arm,encoding='utf8',newline='\n')
    for fixture in ('boot','pas'):
        run(fixture+'-arm64',[a.cxx,'-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-mgeneral-regs-only','-Wall','-Wextra','-Werror',*includes,'-c',str(out/(fixture+'-arm.cpp')),'-o',str(out/(fixture+'.o'))])
    assert 'scmSipCall(1u, arginfo, arg0, arg1, arg2, 0, reply.a6)' in cp
    names=['package/PicassoAdspBoot.cpp','package/PicassoAdspBoot.h','package/PicassoAdspFirmware.cpp',
        'package/PicassoAdspLoadPlan.h','package/PicassoAop.cpp','package/PicassoSmp2pDevice.cpp',
        'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp','drivers/NativeAdrenoPicasso/PicassoPas.h',
        'drivers/NativeAdrenoPicasso/PicassoRpmh.h','scripts/check-picasso-adsp-boot.py',
        'scripts/tests/picasso-adsp-boot-host.cpp','scripts/tests/picasso-pas-host.cpp','scripts/tests/picasso-pas-owner-host.cpp',
        'scripts/build-picasso-display.py','scripts/check-picasso-display-artifacts.py','.github/workflows/host-checks.yml']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,runs=runs,
        inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names}),indent=2)+'\n',encoding='utf8',newline='\n')
    print('PASS production ADSP transaction, PAS protocol and ARM64 API models')


if __name__=='__main__':main()
