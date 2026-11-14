#!/usr/bin/env python3
"""Exercise actual keyboard size wrapper/installation on GNU ObjC, with modeled UIKit consumers."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    source=ROOT/'package/picasso-springboard/picasso-keyboard-size.m'
    src=source.read_text();wrapper=src[:src.index('static void picasso_keyboard_image_added')]
    start=wrapper.index('static void picasso_keyboard_note');end=wrapper.index('static BOOL picasso_keyboard_signature')
    (out/'keyboard-wrapper.inc').write_text(wrapper[:start]+wrapper[end:])
    fixture=ROOT/'scripts/tests/picasso-keyboard-size-objc.m';runs=[]
    for san in [False,True]:
        exe=out/('keyboard-san' if san else 'keyboard')
        cmd=['clang-18','-x','objective-c','-I/usr/lib/gcc/x86_64-linux-gnu/13/include','-I',str(out),'-O1','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror',str(fixture),'-lobjc','-o',str(exe)]
        if san:cmd+=['-fsanitize=address,undefined']
        for stage,command in [('compile',cmd),('run',[str(exe)])]+[(n,[str(exe),n]) for n in ['return','args','last','getter']]:
            q=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
            (out/(exe.name+'-'+stage+'.log')).write_text(q.stdout+q.stderr)
            if q.returncode:raise RuntimeError(q.stdout+q.stderr)
            runs.append(dict(sanitizer=san,stage=stage,passed=True))
    (out/'results.json').write_text(json.dumps(dict(passed=True,runs=runs,scope=__doc__,runtime_qualified=False,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest()),indent=2)+'\n')
    print('PASS keyboard wrapper normal and ASan/UBSan; modeled consumer, exclusions and ABI rejection')
if __name__=='__main__':main()
