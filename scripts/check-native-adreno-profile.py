#!/usr/bin/env python3
"""Build the real VM/submit host fixtures for the opt-in diagnostic experiment."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tree', type=Path, required=True, help='Assembled tree containing AdrenoPM4')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sanitize', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    kit = ROOT / 'drivers/NativeAdreno'
    smmu = ROOT / 'kits/ArmSmmu4K'
    pm4 = args.tree.resolve() / 'kits/AdrenoPM4'
    inc = out / 'include'
    for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h',
                 'IOKit/IOLib.h', 'kern/task.h', 'pexpert/pexpert.h']:
        p = inc / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('#pragma once\n#include "XnuMemoryFixture.h"\n' +
                     ('inline void IOFree(void *p,size_t) {std::free(p); }\n' if name.endswith('IOLib.h') else ''))
    phase = out / 'phase.cpp'
    phase.write_text('#include <cstdint>\nextern "C" { volatile uint64_t native_adreno_maint_phase[8][2]; }\n')
    vm_support = out / 'vm-support.cpp'
    vm_support.write_text('extern "C" int PE_parse_boot_argn(const char *,void *,int) {return 0;}\n')
    submit_support = out / 'submit-support.cpp'
    submit_support.write_text('''#include <cstdint>
extern "C" {
volatile uint32_t native_adreno_hangcheck[8];
volatile uint64_t native_adreno_fence_done[64][2];
void native_adreno_call_start() {}
uint32_t native_adreno_cp_fault_regs(uint32_t *) {return 0;}
uint64_t native_adreno_gpu_ticks() {return 0;}
volatile uint32_t native_gpu_dcvs[15];
unsigned native_pmap_user_mappings(unsigned,void **,uint64_t *,char *) {return 0;}
}
''')
    commands = []
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2', '-g']
    env = dict(os.environ)
    if args.sanitize:
        # The existing suite deliberately retains poisoned/quarantined objects.
        env['ASAN_OPTIONS'] = 'detect_leaks=0'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1'
    with (out / 'result.log').open('w') as log:
        def run(command):
            command = list(map(str, command))
            commands.append(command)
            p = subprocess.run(command, env=env, capture_output=True, text=True)
            log.write(p.stdout + p.stderr)
            log.flush()
            if p.returncode:
                raise RuntimeError((p.stdout + p.stderr)[-4000:])

        c_objects = []
        for name in ['ArmSmmu4KContext', 'ArmSmmu4KLive', 'ArmSmmu4KMmio', 'ArmSmmu4KProgram', 'ArmSmmu4KStreams', 'AdrenoPM4']:
            obj = out / (name + '.o')
            src = (pm4 if name == 'AdrenoPM4' else smmu) / (name + '.c')
            run(['xcrun', 'clang', '-std=c11', *flags, '-c', src, '-o', obj])
            c_objects.append(obj)
        base = ['xcrun', 'clang++', '-std=c++23', *flags, '-I', inc, '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', pm4]
        objects = []
        for name in ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm']:
            obj = out / (name + '.o')
            run([*base, '-c', kit / (name + '.cpp'), '-o', obj])
            objects.append(obj)
        run([*base, kit / 'tests/test_vm.cpp', *objects, *c_objects, phase, vm_support, '-o', out / 'test_vm'])
        run([out / 'test_vm'])
        for name in ['NativeAdrenoRing', 'NativeAdrenoSubmit', 'NativeAdrenoPreempt']:
            obj = out / (name + '.o')
            run([*base, '-c', kit / (name + '.cpp'), '-o', obj])
            objects.append(obj)
        run([*base, kit / 'tests/test_submit.cpp', *objects, *c_objects, phase, submit_support, '-o', out / 'test_submit'])
        run([out / 'test_submit'])
        env['NATIVE_ADRENO_TEST_IB_BATCH'] = '1'
        run([out / 'test_submit'])
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    print((out / 'result.log').read_text()[-2500:])


if __name__ == '__main__':
    main()
