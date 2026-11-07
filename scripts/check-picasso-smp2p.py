#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute SMEM/SMP2P allocation and the production device service body."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cxx', default=shutil.which('clang++'))
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = []

    def run(name, command):
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr, encoding='utf8', newline='\n')
        if r.returncode:
            raise RuntimeError(r.stdout+r.stderr)
        runs.append(dict(name=name, passed=True, output=r.stdout))

    body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (ROOT/'package/PicassoSmp2pDevice.cpp').read_text(), flags=re.M)
    (out/'PicassoSmp2pDevice-body.inc').write_text(body, encoding='utf8', newline='\n')
    includes = [x for d in (out, ROOT/'package', ROOT/'drivers/NativeWlan', ROOT/'drivers/NativeAdrenoPicasso', ROOT/'scripts/tests') for x in ('-I',str(d))]
    core = ROOT/'package/PicassoSmp2p.cpp'
    for sanitized in (False, True):
        suffix = '-asan' if sanitized else ''
        flags = (['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else [])
        for name in ('smp2p','smp2p-device','glink','qrtr','qmi','servloc','tftp','fastrpc','rpc-fs','tfa'):
            exe = out/(name+suffix+('.exe' if os.name=='nt' else ''))
            run(name+'-compile'+suffix, [a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
                str(ROOT/f'scripts/tests/picasso-{name}-host.cpp'),str(core),str(ROOT/'package/PicassoGlink.cpp'),str(ROOT/'package/PicassoRpcFs.cpp'),'-o',str(exe)])
            cases = ('normal',) if name!='smp2p-device' else ('normal','peer','glink','owner','remote','gate','bad',
                'alloc1','alloc2','alloc3','map1','map2','map3','zero1','zero2','zero3')
            for case in cases:
                run(name+'-'+case+suffix,[str(exe),case])
    arm = out/'smp2p-device-arm.cpp'
    arm.write_text('#include "picasso-wlan-kernel-api.h"\nextern "C" {\n'
        'void IOLog(const char *,...) __attribute__((format(printf,1,2)));\n'
        'void IODelay(unsigned); void IOSleep(unsigned); void _disable_preemption(); void _enable_preemption(); }\n'
        '#define disable_preemption() _disable_preemption()\n#define enable_preemption() _enable_preemption()\n'+body,encoding='utf8',newline='\n')
    dma_body = re.sub(r'^#include <[^>]+>[^\n]*\n', '', (ROOT/'package/PicassoRpcDma.cpp').read_text(), flags=re.M)
    dma = out/'rpc-dma-arm.cpp'
    dma.write_text('#include "picasso-wlan-kernel-api.h"\n'
        'extern "C" void IOLog(const char *,...) __attribute__((format(printf,1,2)));\n'
        'using IOOptionBits=unsigned long long;using IOByteCount=unsigned long long;extern void *kernel_task;\n'
        'enum { kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=16,kIOMemoryMapperNone=32 };\n'
        'class IOBufferMemoryDescriptor {public: static IOBufferMemoryDescriptor *inTaskWithOptions(void *,IOOptionBits,unsigned,unsigned);'
        'void *getBytesNoCopy();uint64_t getPhysicalSegment(unsigned,IOByteCount *,unsigned);};\n'+dma_body,encoding='utf8',newline='\n')
    for name,source in [('smp2p',core),('smp2p-device',arm),('glink',ROOT/'package/PicassoGlink.cpp'),('rpc-dma',dma),('rpc-fs',ROOT/'package/PicassoRpcFs.cpp')]:
        run(name+'-arm64',[a.cxx,'-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding',
            '-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-mgeneral-regs-only','-Wall','-Wextra','-Werror',
            '-Wframe-larger-than=2048',
            *includes,'-c',str(source),'-o',str(out/(name+'.o'))])
    # Run the actual allocator/adapter body; only ARM cache/barrier instructions
    # are replaced with a host W1C/barrier model. No copied prepare algorithm.
    host_dma = re.sub(r'__asm__ volatile\([^;]+;', 'dmaBarrier();', dma_body)
    assert host_dma.count('dmaBarrier();') == 3
    (out/'PicassoRpcDma-body.inc').write_text(host_dma, encoding='utf8', newline='\n')
    for sanitized in (False, True):
        suffix = '-asan' if sanitized else ''
        flags = ['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
        exe=out/('pcm-device'+suffix+('.exe' if os.name=='nt' else ''))
        run('pcm-device-compile'+suffix,[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*flags,*includes,
            str(ROOT/'scripts/tests/picasso-pcm-device-host.cpp'),str(ROOT/'package/PicassoRpcFs.cpp'),'-o',str(exe)])
        for case in range(16):run('pcm-device-'+str(case)+suffix,[str(exe),str(case)])
    inputs = ['scripts/tests/picasso-pcm-device-host.cpp','scripts/tests/picasso-rpc-fs-host.cpp','scripts/make-picasso-rpc-stock.py','package/PicassoRpcFs.h','package/PicassoRpcFs.cpp','package/PicassoRpcStock.inc','package/PicassoFastrpc.h','package/PicassoPcm.h','package/PicassoRpcDma.h','package/PicassoRpcDma.cpp','scripts/tests/picasso-fastrpc-host.cpp','package/PicassoTftp.h','scripts/tests/picasso-tftp-host.cpp','package/PicassoServloc.h','scripts/tests/picasso-servloc-host.cpp','package/PicassoSmp2p.h','package/PicassoSmp2p.cpp','package/PicassoSmp2pDevice.cpp',
        'package/PicassoGlink.h','package/PicassoGlink.cpp','package/PicassoApr.h','package/PicassoAudioPins.h','package/PicassoTfa.h','scripts/tests/picasso-tfa-host.cpp','package/PicassoQrtr.h','package/PicassoQmi.h','scripts/tests/picasso-glink-host.cpp','scripts/tests/picasso-qrtr-host.cpp','scripts/tests/picasso-qmi-host.cpp',
        'package/PicassoSmemLock.h','package/PicassoAdspBoot.h','package/PicassoAdsp.h','drivers/NativeWlan/WlanSmem.h','drivers/NativeAdrenoPicasso/PicassoRpmh.h',
        'scripts/check-picasso-smp2p.py','scripts/tests/picasso-smp2p-host.cpp','scripts/tests/picasso-smp2p-device-host.cpp',
        'scripts/tests/picasso-wlan-kernel-api.h','scripts/build-picasso-display.py',
        'scripts/check-picasso-display-artifacts.py','.github/workflows/host-checks.yml']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,
        dsp_started=False,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}),indent=2)+'\n',encoding='utf8',newline='\n')
    print('PASS SMEM locked allocation, SMP2P service and ARM64 API model')


if __name__=='__main__':
    main()
