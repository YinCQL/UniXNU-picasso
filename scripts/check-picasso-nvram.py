#!/usr/bin/env python3
"""Exercise real CHRP validation/restore with host object and lock models."""
import argparse,importlib.util,json,os,subprocess,hashlib,runpy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def load(n):
    spec=importlib.util.spec_from_file_location(n,ROOT/'scripts'/(n+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--xnu',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    source=(a.xnu/'iokit/Kernel/IONVRAMCHRPHandler.cpp').read_text()
    extract=load('check-picasso-buttons').extract
    names=['static const char *\nget_bank_version_string(', 'static uint32_t\nadler32(', 'static uint32_t\nnvram_get_adler(',
           'static uint32_t\nadler32_with_version(', 'static uint8_t\nchrp_checksum(', 'static IOReturn\nnvram_validate_header_v1v2(', 'static NVRAMVersion\nvalidateNVRAMVersion(']
    (out/'nvram-helpers.inc').write_text('\n'.join(extract(source,n) for n in names).replace('offsetof(struct chrp_nvram_header, data)','sizeof(struct chrp_nvram_header)'))
    (out/'nvram-restore.inc').write_text(extract(source,'IOReturn IONVRAMCHRPHandler::restorePicasso('))
    cache=(a.xnu/'bsd/kern/kern_exec.c').read_text()
    start=cache.index('#define PICASSO_PIPE_BLOCK')
    end=cache.index('/* Sequential reader over the stream',start)
    (out/'cache-writer.inc').write_text(cache[start:end])
    console=(a.xnu/'bsd/dev/picasso_usb.c').read_text()
    # The declaration precedes the complete definition; select the latter.
    (out/'console-snapshot.inc').write_text(extract(console,'uint32_t picasso_console_snapshot(void *out'))
    generator=runpy.run_path(str(ROOT/'scripts/make-picasso-adt.py'));bank=dict(generator['empty_nvram_proxy']())['nvram-proxy-data']
    (out/'proxy.bin').write_bytes(bank)
    host=load('check-picasso-user-fault');runs=[]
    for asan in (False,True):
        n='nvram'+('-asan' if asan else '')
        if os.name=='nt':
            cmd=f'cl /nologo /std:c++17 /EHsc /W4 /WX /wd4100 /Od /Zi /I"{out}" /Fd:{n}.pdb /Fo:{n}.obj /Fe:{n}.exe "{ROOT/"scripts/tests/picasso-nvram-host.cpp"}"'
            if asan:cmd+=' /fsanitize=address /link /INCREMENTAL:NO'
            result=host.batch_run(out,n,cmd+f' > {n}-compile.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n{n}.exe proxy.bin > {n}.log 2>&1\nexit /b %errorlevel%\n',host.find_vcvars())
        else:
            cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-O1','-g','-I',str(out),str(ROOT/'scripts/tests/picasso-nvram-host.cpp'),'-o',str(out/n)]
            if asan:cmd+=['-fsanitize=address,undefined']
            result=subprocess.run(cmd,capture_output=True,text=True);(out/(n+'-compile.log')).write_text(result.stdout+result.stderr)
            if not result.returncode:
                result=subprocess.run([str(out/n),str(out/'proxy.bin')],capture_output=True,text=True);(out/(n+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(f'Failed {n}, see {out}')
        runs.append(dict(name=n,passed=True,log=(out/(n+'.log')).read_text()))
    for asan in (False,True):
        n='cache'+('-asan' if asan else '')
        if os.name=='nt':
            cmd=f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi /I"{out}" /Fd:{n}.pdb /Fo:{n}.obj /Fe:{n}.exe "{ROOT/"scripts/tests/picasso-cache-host.cpp"}"'
            if asan:cmd+=' /fsanitize=address /link /INCREMENTAL:NO'
            result=host.batch_run(out,n,cmd+f' > {n}-compile.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n{n}.exe > {n}.log 2>&1\nexit /b %errorlevel%\n',host.find_vcvars())
        else:
            cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-I',str(out),str(ROOT/'scripts/tests/picasso-cache-host.cpp'),'-o',str(out/n)]
            if asan:cmd+=['-fsanitize=address,undefined']
            result=subprocess.run(cmd,capture_output=True,text=True);(out/(n+'-compile.log')).write_text(result.stdout+result.stderr)
            if not result.returncode:
                result=subprocess.run([str(out/n)],capture_output=True,text=True);(out/(n+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(f'Failed {n}, see {out}')
        runs.append(dict(name=n,passed=True,log=(out/(n+'.log')).read_text()))
    (out/'results.json').write_text(json.dumps(dict(passed=True,scope=__doc__,runs=runs,source_sha256=hashlib.sha256(source.encode()).hexdigest()),indent=2)+'\n')
    print('PASS: NVRAM original CHRP helpers and restore method')
if __name__=='__main__':main()
