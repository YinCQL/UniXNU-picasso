#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise reviewed PR19 probes/abort capture and Linux host compiler isolation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from importlib.util import spec_from_file_location, module_from_spec

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--cxx', default=shutil.which('clang++') or 'C:/Program Files/LLVM/bin/clang++.exe')
    p.add_argument('--adt', type=Path)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    spec = spec_from_file_location('extract', ROOT/'scripts/check-picasso-gop-handoff.py')
    mod = module_from_spec(spec); spec.loader.exec_module(mod)
    display = (ROOT/'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp').read_text()
    methods = '\n'.join(mod.extract(display, x) for x in ('    static uint32_t sdeRead(', '    void sdeProbe()', '    void sdeDiag()'))
    source = (ROOT/'scripts/tests/picasso-pr19-probe.cpp').read_text().replace('// PROBE_METHODS', methods)
    (out/'probe.cpp').write_text(source, encoding='utf8', newline='\n')
    patch = (ROOT/'patches/picasso-abort-text-xnu.patch').read_text()
    added = '\n'.join(x[1:] for x in patch.splitlines() if x.startswith('+') and not x.startswith('+++'))
    helper = mod.extract(added, 'static void\npicasso_abort_payload_text(')
    (out/'abort.cpp').write_text((ROOT/'scripts/tests/picasso-pr19-abort.c').read_text().replace('// ABORT_HELPER', helper), encoding='utf8', newline='\n')
    runs = []

    def run(name, command, **kw):
        r = subprocess.run(command, capture_output=True, text=True, timeout=120, **kw)
        (out/(name+'.log')).write_text(r.stdout+r.stderr, encoding='utf8', newline='\n')
        if r.returncode: raise RuntimeError(name+'\n'+r.stdout+r.stderr)
        runs.append(dict(name=name, passed=True))
        return r.stdout

    for sanitized in (False, True):
        suffix = '-asan' if sanitized else ''
        flags = (['-fsanitize=address' if os.name=='nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else [])
        for fixture in ('probe', 'abort'):
            exe = out/(fixture+suffix+('.exe' if os.name=='nt' else ''))
            run(fixture+'-compile'+suffix, [a.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags, str(out/(fixture+'.cpp')), '-o', str(exe)])
            cases = ['normal'] if fixture=='abort' else ['normal','disabled','no-boot','bad-pages','64k-pages','bad-bank',
                'zero-banks','too-many-banks','outside-map',*['buffer'+str(n) for n in range(1,4)],
                *['alloc'+str(n) for n in range(1,4)],*['map'+str(n) for n in range(1,4)],*['zero'+str(n) for n in range(1,4)]]
            for case in cases:
                output = run(fixture+'-'+case+suffix, [str(exe),case])
                if fixture=='probe':
                    if case in ('disabled','bad-pages','64k-pages','bad-bank','zero-banks','too-many-banks','outside-map','zero3'):
                        assert 'PICASSO_SMMU cbar=' not in output, case
                    elif case=='normal':
                        assert 'PICASSO_SMMU cbar=' in output and output.count(' ok pa=180000000 ')==3
    arm = '''#include <stdint.h>
#include <stddef.h>
extern "C" int printf(const char *,...) __attribute__((format(printf,1,2)));
extern bool PE_parse_boot_argn(const char *,void *,int);
using vm_size_t=uintptr_t;using IOOptionBits=uint32_t;using IOByteCount=uintptr_t;using addr64_t=uint64_t;
enum {kIODirectionIn=1,kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=0x10,kIOMapAnywhere=1,kIOMapInhibitCache=2};
struct IOMemoryMap {uintptr_t getVirtualAddress();uint64_t getPhysicalAddress();uint64_t getLength();void release();};
struct IOMemoryDescriptor {static IOMemoryDescriptor *withPhysicalAddress(uint64_t,uint64_t,unsigned);IOMemoryMap *map(unsigned);void release();};
struct IOBufferMemoryDescriptor {static IOBufferMemoryDescriptor *withOptions(IOOptionBits,vm_size_t,vm_size_t);
addr64_t getPhysicalSegment(IOByteCount,IOByteCount *,IOOptionBits);IOByteCount getLength();void release();};
#define OSSafeReleaseNULL(p) do {if(p){p->release();p=nullptr;}}while(0)
struct Probe { IOMemoryMap *bootMapping;uint32_t panelWidth,panelHeight;
'''+methods+'\n};\nvoid check(Probe *p){p->sdeProbe();p->sdeDiag();}\n'
    (out/'probe-arm.cpp').write_text(arm, encoding='utf8', newline='\n')
    run('probe-arm64', [a.cxx,'-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Werror','-c',str(out/'probe-arm.cpp'),'-o',str(out/'probe.o')])
    if sys.platform.startswith('linux'):
        if not a.adt: raise ValueError('Linux isolation regression needs --adt')
        workflow = (ROOT/'.github/workflows/build-kernel-linux.yml').read_text()
        # Execute the same PATH restoration used by both post-build check steps.
        restore = 'export PATH="$PICASSO_HOST_PATH"'
        assert workflow.count(restore)==2 and 'echo "PICASSO_HOST_PATH=$PATH" >> "$GITHUB_ENV"' in workflow
        poison = out/'target-only'; poison.mkdir()
        for name in ('clang','clang++','gcc','g++'):
            q=poison/name;q.write_text('#!/bin/sh\necho "error: unknown target triple unknown" >&2\nexit 99\n');q.chmod(0o755)
        env=dict(os.environ, PICASSO_HOST_PATH=os.environ['PATH'], PATH=str(poison)+os.pathsep+os.environ['PATH'])
        wrong=subprocess.run(['clang++','--version'],env=env,capture_output=True)
        assert wrong.returncode==99
        run('linux-host-path', ['/bin/bash','-c',restore+'\nexec "$@"','--',sys.executable,'-B',str(ROOT/'scripts/check-picasso-display-policy.py'),'--adt',str(a.adt.resolve()),'--output',str(out/'isolated-policy')],env=env)
    inputs=['scripts/check-picasso-pr19.py','scripts/tests/picasso-pr19-probe.cpp','scripts/tests/picasso-pr19-abort.c',
            'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp','drivers/NativeMobileDisplay/NativeGOPPresenter.cpp',
            'drivers/NativeCpuFreq/CpuFreqProbe.cpp','drivers/NativeNovatekTouch/TouchProbe.cpp',
            'drivers/NativeMobileDisplay/boot-progress/BootProgress.c','drivers/NativeMobileDisplay/boot-progress/PicassoBootProgressStubs.c',
            'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp','patches/picasso-gpu-table-xnu.patch','patches/picasso-abort-text-xnu.patch',
            'patches/picasso-settings-waits-xnu.patch','scripts/check-picasso-display-policy.py','.github/workflows/build-kernel-linux.yml','unixnu']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,kernel_sdk_compiled=False,runs=runs,
        linux_host_isolation_tested=sys.platform.startswith('linux'),inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs}),indent=2)+'\n',encoding='utf8',newline='\n')
    print('PASS reviewed PR19 read-only probes and bounded diagnostics; Linux host isolation '+('passed' if sys.platform.startswith('linux') else 'requires Linux'))


if __name__=='__main__':main()
