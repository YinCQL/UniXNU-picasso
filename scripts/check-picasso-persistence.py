#!/usr/bin/env python3
"""Execute patched RamBlock/dual-record storage with faulted host media; no device I/O."""
import argparse, hashlib, importlib.util, json, os
from pathlib import Path
import shutil, subprocess, re
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    source=a.tree/'drivers/RamBlock'
    if not (source/'PersistOverlay.h').is_file():source=a.tree
    for n in ('PersistOverlay.h','DirectDisk.h','SeedDisk.h','SparseDisk.h','NativeRamBlock.cpp'):
        shutil.copyfile(source/n,out/n)
    if 'initializeBlank' not in (out/'PersistOverlay.h').read_text():
        subprocess.run(['git','-c','core.autocrlf=false','init','-q',str(out)],check=True)
        subprocess.run(['git','-c','core.autocrlf=false','apply','-'],cwd=out,
            input=(ROOT/'patches/picasso-native-ram-block-mode1-guard.patch').read_text().encode(),check=True)
    spec=importlib.util.spec_from_file_location('host',ROOT/'scripts/check-picasso-user-fault.py')
    host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    extract_spec=importlib.util.spec_from_file_location('extract',ROOT/'scripts/check-picasso-buttons.py')
    extractor=importlib.util.module_from_spec(extract_spec);extract_spec.loader.exec_module(extractor)
    ram=(out/'NativeRamBlock.cpp').read_text()
    (out/'sync.inc').write_text(extractor.extract(ram,'  IOReturn doSynchronizeCache()').replace(' override',''))
    for name,signature in [('fallback-qualify','  static bool acceptsVolatileFallback('),
                           ('fallback-allocate','  uint8_t *allocateSparsePage('),
                           ('fallback-prepare','  bool prepareVolatileFallback(')]:
        (out/(name+'.inc')).write_text(extractor.extract(ram,signature))
    # Replay the real large-seed import and worker, including its declaration of
    # sparse.bytes. Hand-constructed fallback state missed this in build163.
    for name,signature in [('seed-import','  if(packedInput){'),
                           ('fallback-attach','  void attachStore('),
                           ('fallback-worker','  static void startPersistent('),
                           ('fallback-available','  bool available('),
                           ('fallback-seed-info','  bool seedInfo(')]:
        (out/(name+'.inc')).write_text(extractor.extract(ram,signature))
    (out/'aux-claim.inc').write_text(extractor.extract((ROOT/'drivers/NativeUfs/PicassoPersistence.h').read_text(),'static bool picassoAuxClaim('))
    runs=[]
    for sanitized in (False,True):
        name='storage'+('-asan' if sanitized else '')
        if os.name=='nt':
            cmd=f'cl /nologo /std:c++17 /EHsc /W4 /WX /wd4505 /Od /Zi /I"{out}" /I"{ROOT / "drivers/NativeUfs"}" /Fd:{name}.pdb /Fo:{name}.obj /Fe:{name}.exe "{ROOT / "scripts/tests/picasso-persistence-host.cpp"}"'
            if sanitized:cmd+=' /fsanitize=address /link /INCREMENTAL:NO'
            result=host.batch_run(out,name,cmd+f' > {name}-compile.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n{name}.exe > {name}.log 2>&1\nexit /b %errorlevel%\n',host.find_vcvars())
        else:
            cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g','-I',str(out),'-I',str(ROOT/'drivers/NativeUfs'),str(ROOT/'scripts/tests/picasso-persistence-host.cpp'),'-o',str(out/name)]
            if sanitized:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            result=subprocess.run(cmd,capture_output=True,text=True)
            (out/(name+'-compile.log')).write_text(result.stdout+result.stderr)
            if not result.returncode:
                result=subprocess.run([str(out/name)],capture_output=True,text=True)
                (out/(name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(f'Failed {name}; see {out}')
        runs.append(dict(name=name,passed=True,log=(out/(name+'.log')).read_text()))
    cc=shutil.which('clang++') or (r'C:/Program Files/LLVM/bin/clang++.exe' if os.name=='nt' else None)
    api=out/'arm-api';api.mkdir()
    (api/'string.h').write_text('#pragma once\n#include <stddef.h>\nextern "C" {void *memcpy(void *,const void *,size_t);void *memset(void *,int,size_t);int memcmp(const void *,const void *,size_t);int strcmp(const char *,const char *);size_t strlen(const char *); }\n')
    body=out/'records-kernel.cpp'
    body.write_text('#include "picasso-records-kernel-api.h"\n#include "PicassoPersistence.h"\nbool qualification(uint64_t first,uint64_t blocks){return picassoAuxClaim(first,blocks); }\n')
    cmd=[cc,'-target','arm64-apple-ios17.0','-std=c++17','-ffreestanding','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-mgeneral-regs-only','-Wall','-Wextra','-Werror',
         '-I',str(api),'-I',str(ROOT/'scripts/tests'),'-I',str(ROOT/'drivers/NativeUfs'),'-c',str(body),'-o',str(out/'records-kernel.o')]
    result=subprocess.run(cmd,capture_output=True,text=True)
    (out/'records-kernel.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(f'Failed ARM64 records integration; see {out}')
    ram_source=re.sub(r'^#include <[^>]+>[^\n]*\n','',(out/'NativeRamBlock.cpp').read_text(),flags=re.M)
    ram_body=out/'ram-kernel.cpp';ram_body.write_text('#include "picasso-ramblock-kernel-api.h"\n'+ram_source)
    ram_cmd=cmd[:];ram_cmd[ram_cmd.index(str(body))]=str(ram_body);ram_cmd[ram_cmd.index(str(out/'records-kernel.o'))]=str(out/'ram-kernel.o');ram_cmd+=['-Wno-unused-function']
    result=subprocess.run(ram_cmd,capture_output=True,text=True)
    (out/'ram-kernel.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(f'Failed ARM64 RamBlock integration; see {out}')
    (out/'results.json').write_text(json.dumps(dict(scope=__doc__,passed=True,runs=runs,hardware_tested=False,source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'drivers/NativeUfs/PicassoRecord.h',ROOT/'drivers/NativeUfs/PicassoPersistence.h',ROOT/'patches/picasso-native-ram-block-mode1-guard.patch',ROOT/'scripts/tests/picasso-persistence-host.cpp']}),indent=2)+'\n')
    print('PASS: 4 GiB direct Data and double-buffered persistent records')
if __name__=='__main__':main()
