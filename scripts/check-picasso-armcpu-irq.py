#!/usr/bin/env python3
"""Exercise actual ArmCpu registration with host lock/MMIO stand-ins.

Checks publication and register updates under concurrent registration. Hardware
GIC routing/delivery and target assembly are qualified separately.
"""
import argparse, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def extract(source, signature):
    begin=source.index(signature); opening=source.index('{',begin); depth=1; end=opening+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    return source[begin:end]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=False)
    p=ROOT/'drivers/ArmCpu/ArmCpu.cpp'; source=p.read_text()
    begin=source.index('#define ARMCPU_SPI_SLOTS'); end=source.index('extern "C" {',begin)
    helpers=source[begin:end]
    helpers+='\n'+extract(source,'static uint64_t gicdRouterOffset')
    helpers+='\n'+extract(source,'static uint64_t gicdRouteFromAffinity')
    for signature in ['static IOReturn spiAttachUnlocked','extern "C" IOReturn armcpu_spi_attach','extern "C" IOReturn armcpu_spi_mask','extern "C" uint64_t armcpu_spi_count']:
        helpers+='\n'+extract(source,signature)
    helpers=re.sub(r'__asm__ volatile \("dsb sy" ::: "memory"\);','__atomic_thread_fence(__ATOMIC_SEQ_CST);',helpers)
    assert '__asm__' not in helpers
    (out/'registration.inc').write_text(helpers+'\n')
    fixture=ROOT/'scripts/tests/picasso-armcpu-irq-host.cpp'
    shutil.copyfile(fixture,out/'fixture.cpp')
    cc=shutil.which('clang++') or shutil.which('g++')
    if not cc: raise RuntimeError('No host C++ compiler')
    runs=[]
    for name,sanitizer in [('ordinary',False),('asan',True)]:
        exe=out/name; command=[cc,'-std=c++17','-O1','-g','-pthread','-Wall','-Wextra',str(out/'fixture.cpp'),'-I',str(ROOT/'drivers/ArmCpu'),'-o',str(exe)]
        if sanitizer: command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        with (out/(name+'-compile.log')).open('w') as log: result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode: raise RuntimeError('Compilation failed: '+str(out/(name+'-compile.log')))
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        with (out/(name+'-run.log')).open('w') as log: result=subprocess.run([str(exe)],env=env,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode or 'PASS ArmCpu' not in (out/(name+'-run.log')).read_text(): raise RuntimeError('Fixture failed: '+str(out/(name+'-run.log')))
        runs.append(dict(name=name,passed=True))
    result=dict(passed=True,inputs={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest(),str(fixture.relative_to(ROOT)):hashlib.sha256(fixture.read_bytes()).hexdigest(),'drivers/ArmCpu/ArmCpuTimer.h':hashlib.sha256((ROOT/'drivers/ArmCpu/ArmCpuTimer.h').read_bytes()).hexdigest()},runs=runs,concurrent_distinct_rounds=64,concurrent_duplicate_rounds=64,threads=16,hardware_tested=False,scope='actual registration code; host IOLock/MMIO stand-ins')
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS ArmCpu: 64 distinct + 64 duplicate 16-thread rounds, boundary/timeout/publication/owned SGI; ordinary + ASan/UBSan')
if __name__=='__main__': main()
