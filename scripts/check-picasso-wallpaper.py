#!/usr/bin/env python3
"""Verify bounded pixel sampling and real wallpaper wrapper dispatch; no Apple image or device qualification."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    src=(ROOT/'package/picasso-springboard/picasso-wallpaper-probe.m').read_text()
    wrappers='static uintptr_t picasso_wallpaper_uikit_base;\n'+src[src.index('typedef id'):src.index('static void picasso_wallpaper_note')]
    wrappers+=src[src.index('static unsigned picasso_wallpaper_ticket'):src.index('static void picasso_wallpaper_image_added')]
    (out/'wallpaper-wrapper.inc').write_text(wrappers)
    (out/'wallpaper-content.inc').write_text(src[src.index('static IOSurfaceRef picasso_wallpaper_image_surface'):src.index('static void picasso_wallpaper_observe_surface')])
    (out/'wallpaper-image.inc').write_text(src[src.index('static void picasso_wallpaper_image_added'):])
    (out/'wallpaper-budget.inc').write_text(src[src.index('static int picasso_wallpaper_take'):src.index('/* This getter returns an IOSurface-backed UIImage')])
    src=(ROOT/'package/picasso-springboard/picasso-astc-runtime.m').read_text()
    guard=(ROOT/'package/picasso-springboard/picasso-astc-guard.m').read_text()
    (out/'wallpaper-hash.inc').write_text(guard[guard.index('static int picasso_astc_hash'):guard.index('static void picasso_astc_image_added')])
    (out/'astc-note.inc').write_text(src[src.index('static void picasso_astc_decode_note'):src.index('static int picasso_astc_guard')])
    runs=[]
    for fixture in ['picasso-wallpaper-sample-host.c','picasso-wallpaper-objc.m','picasso-wallpaper-image-host.m','picasso-wallpaper-surface-host.c','picasso-wallpaper-content-host.m']:
      for san in [False,True]:
        exe=out/(Path(fixture).stem+('-san' if san else '-normal'))
        cmd=['clang-18','-O1','-Wall','-Wextra','-Werror','-I',str(out),'-I',str(ROOT/'package/picasso-springboard'),str(ROOT/'scripts/tests'/fixture),'-o',str(exe)]
        if fixture.endswith('.m'):cmd+=['-I/usr/lib/gcc/x86_64-linux-gnu/13/include','-lobjc']
        if fixture.endswith('image-host.m'):cmd+=['-fblocks','-lcrypto']
        if san:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        commands=[('compile',cmd),('run',[str(exe)])]
        if fixture=='picasso-wallpaper-objc.m':commands += [(mode,[str(exe),mode]) for mode in ['return','error','composite-abi','snapshot-abi','missing-method','different-imp','missing-class','home-abi','home-imp','home-missing']]
        if fixture=='picasso-wallpaper-content-host.m':commands += [(mode,[str(exe),mode]) for mode in ['offset','ivar-type','abi','missing','imp','nil','class','unqualified']]
        for stage,command in commands:
          q=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
          (out/(exe.name+'-'+stage+'.log')).write_text(q.stdout+q.stderr)
          if q.returncode:raise RuntimeError(q.stdout+q.stderr)
          runs.append(dict(fixture=fixture,sanitized=san,stage=stage,passed=True,output=q.stdout))
    inputs=['package/picasso-springboard/'+n for n in ['picasso-wallpaper-probe.m','picasso-pixel-sample.h','picasso-wallpaper-surface.h','picasso-astc-runtime.m','picasso-astc-guard.m','picasso-astc-guard-core.h','picasso-ui-runtime.m']]
    inputs+=['scripts/'+n for n in ['check-picasso-wallpaper.py','picasso_runtime_ui.py','tests/picasso-wallpaper-sample-host.c','tests/picasso-wallpaper-objc.m','tests/picasso-wallpaper-image-host.m','tests/picasso-wallpaper-surface-host.c','tests/picasso-wallpaper-content-host.m']]
    (out/'results.json').write_text(json.dumps(dict(passed=True,scope=__doc__,runs=runs,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs},apple_provider_sampling_qualified=False,gnu_runtime_leak_scan=False),indent=2)+'\n')
    print('PASS wallpaper pixel sampling and actual ObjC wrappers, normal + ASan/UBSan')
if __name__=='__main__':main()
