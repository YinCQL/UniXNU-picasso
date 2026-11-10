from pathlib import Path
import subprocess,hashlib,json
import argparse
r=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=(r/'drivers/NativeGL/eagl/eagl.c').read_text()
first=s[s.index('static bool\nmirror_to_surface'):s.index('\n/*\n * For looking at what is drawn')]
start=s.index('frame_end(struct eagl *e, bool finished)')
body=s[s.index('\tbefore = now_us();',start):s.index('\n\tif (!e->in_frame)',start)]
(out/'gl-handoff.inc').write_text(first+'\nstatic void frame_handoff(struct eagl *e,bool finished){\nunsigned copied=0,in_place=0;bool mirror_queued=false;uint64_t before,waited;\n'+body+'\n(void)copied;(void)before;(void)waited;\n}\n')
source=r/'scripts/tests/gl-handoff-host.c';runs=[]
for flags,name in [([], 'normal'),(['-fsanitize=address,undefined','-fno-omit-frame-pointer'],'asan')]:
 exe=out/name
 cmd=['clang-18','-O2','-Wall','-Wextra','-Werror',*flags,'-I',str(out),str(source),'-o',str(exe)]
 for n,c in [('compile',cmd),('run',[str(exe)])]:
  p=subprocess.run(c,capture_output=True,text=True);(out/(name+'-'+n+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr;runs.append(dict(name=name+'-'+n,passed=True,output=p.stdout))
(out/'results.json').write_text(json.dumps(dict(passed=True,runtime_qualified=False,source_sha256=hashlib.sha256(s.encode()).hexdigest(),runs=runs),indent=2)+'\n');print(runs[-1]['output'])
