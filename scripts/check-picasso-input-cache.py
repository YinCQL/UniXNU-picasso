#!/usr/bin/env python3
"""Exercise the input-cache wrapper and exact runtime UI addon boundaries."""
from pathlib import Path
import argparse, json, subprocess, importlib.util, os
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xnu',type=Path,required=True)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    src=(ROOT/'package/picasso-springboard/picasso-input-cache.m').read_text()
    wrapper=src[:src.index('static void picasso_input_image_added')]
    start=wrapper.index('static void picasso_input_note(');end=wrapper.index('__attribute__((noinline))')
    (out/'input-wrapper.inc').write_text(wrapper[:start]+wrapper[end:])
    runs=[]
    for san in [False,True]:
        exe=out/('wrapper-san' if san else 'wrapper')
        cmd=['clang-18','-x','objective-c','-I/usr/lib/gcc/x86_64-linux-gnu/13/include','-I',str(out),'-O1','-fno-omit-frame-pointer','-fno-optimize-sibling-calls','-Wall','-Wextra','-Werror','-Wno-frame-address',str(ROOT/'scripts/tests/picasso-input-cache-objc.m'),'-lobjc','-o',str(exe)]
        if san:cmd+=['-fsanitize=address,undefined']
        for stage,command in [('compile',cmd),('run',[str(exe)]),('return-abi',[str(exe),'return']),('args-abi',[str(exe),'args'])]:
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0') # GNU runtime process-global class caches.
            q=subprocess.run(command,capture_output=True,text=True,env=env)
            (out/(exe.name+'-'+stage+'.log')).write_text(q.stdout+q.stderr)
            if q.returncode:raise RuntimeError(q.stdout+q.stderr)
            runs.append(dict(sanitizer=san,stage=stage,passed=True))
    spec=importlib.util.spec_from_file_location('buttons',ROOT/'scripts/check-picasso-buttons.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    x=(a.xnu/'bsd/kern/kern_exec.c').read_text()
    funcs='\n'.join(m.extract(x,n) for n in ['static bool\npicasso_audio_addon_entry(', 'static bool\npicasso_media_addon_entry('])
    import picasso_media_payload as media, picasso_runtime_ui as ui
    addon,trust,report,*_=media.make(a.work.resolve());files=media.unpack(addon)
    table='\n'.join('{'+json.dumps(n)+','+str(mode)+'},' for n,mode,_ in files)
    test='#include <stdbool.h>\n#include <stdint.h>\n#include <string.h>\n#include <assert.h>\n'+funcs+'\nstruct row {const char *name;uint32_t mode;};\nstatic struct row rows[]={'+table+'};\n'
    test+=r'''int main(void){
      for(unsigned i=0;i<13;++i){
        assert(picasso_media_addon_entry(13,i,rows[i].name,rows[i].mode,true));
        assert(!picasso_media_addon_entry(13,i,rows[i].name,rows[i].mode,false));
        assert(!picasso_media_addon_entry(13,i,rows[i].name,0777,true));
        assert(!picasso_media_addon_entry(13,i,"ui/../bad",rows[i].mode,true));
      }
      for(unsigned i=0;i<12;++i)assert(picasso_media_addon_entry(12,i,rows[i].name,rows[i].mode,true));
      assert(!picasso_media_addon_entry(12,12,rows[12].name,0644,true));
      assert(!picasso_media_addon_entry(13,13,rows[12].name,0644,true));
      assert(!picasso_media_addon_entry(13,12,0,0644,true));
      assert(!picasso_media_addon_entry(14,12,rows[12].name,0644,true));
      return 0;}
'''
    (out/'whitelist.c').write_text(test)
    for san in [False,True]:
        exe=out/('whitelist-san' if san else 'whitelist')
        cmd=['clang-18','-Wall','-Wextra','-Werror',str(out/'whitelist.c'),'-o',str(exe)]
        if san:cmd+=['-fsanitize=address,undefined']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
    lib,build=ui.verify(a.work.resolve());assert files[-1]==(ui.FILE,0o644,lib)
    import picasso_audio_payload as audio
    old=media.modules(audio.make(a.work.resolve())[0]);new=media.modules(trust)
    assert old[:2]==new[:2] and old[3]==new[3] and old[2]!=new[2]
    assert new[2][24:44].hex()==build['identity']['cdhash'] and len(new[2])==48
    from picasso_ui_trust import library_identity
    bad=bytearray(lib);bad[500]^=1
    try:library_identity(bytes(bad))
    except ValueError:pass
    else:raise AssertionError('Corrupt UI code accepted')
    (out/'results.json').write_text(json.dumps(dict(passed=True,runs=runs,exact_runtime_ui=True,original_and_audio_modules_unchanged=True,gnu_runtime_leak_scan=False,runtime_qualified=False,library=build['identity']),indent=2)+'\n')
    print('PASS input-cache wrapper, addon boundaries, exact runtime UI trust, corrupted code rejection')
if __name__=='__main__':main()
