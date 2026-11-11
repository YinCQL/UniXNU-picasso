#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check production group state restoration using real GL and a named shader surrogate."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--source',type=Path,default=ROOT/'drivers/NativeGL/eagl/eagl.c')
a=p.parse_args();s=a.source.read_text();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
def fn(name):
    m=re.search(r'static [^\n]+\n'+name+r'\(',s);assert m,name
    return s[m.start():s.index('\n}',m.start())+2]+'\n'
header=s[s.index('typedef unsigned int GLenum'):s.index('struct eagl;',s.index('typedef unsigned int GLenum'))]
for define in re.findall(r'^#define GL_\w+\s+[^\n]+',s,re.M):
    if define.split()[1] not in header:header+=define+'\n'
decl=(ROOT/'out/picasso/nativegl/eagl_gen.h').read_text().split('#ifdef EAGL_GEN_DECLARATIONS\n')[1].split('#endif')[0]
body=header+'\n'+decl+'''
enum {GROUP_NONE,GROUP_FRESH,GROUP_VIBRANT};
struct group_texture {unsigned state;GLfloat colours[20];};
struct eagl {GLuint reader,framebuffer,program,bound[8],unit,group_cover,group_over,group_flat,colour;
bool group_failed;unsigned frames,draw_function;
GLint group_cover_matrix,group_cover_texmat,group_over_matrix,group_over_texmat,group_over_colours,group_flat_matrix,group_flat_colour;
struct {const void *pointer;GLsizei stride;GLint size;GLenum type;} arrays[8];};
static unsigned group_flattened,captured_lines;
static char captured[200];
static bool compositor_claim(unsigned b,unsigned k){(void)b;(void)k;return false;}
static void compositor_text(const char *fmt,...){va_list ap;va_start(ap,fmt);vsnprintf(captured,sizeof(captured),fmt,ap);va_end(ap);captured_lines++;}
static void compositor_program(unsigned p,int g,const char *s){(void)p;(void)g;(void)s;}
static void say(const char *fmt,...){(void)fmt;}
'''
for name in ['plain_vertex','group_cover_fragment','group_over_fragment','group_flat_fragment']:
    at=s.index('static const char '+name);end=re.search(r'";\n',s[at:]);assert end
    body+=s[at:at+end.end()]+'\n'
for name in ['plain_program','clamp01','group_programs','compositor_framebuffer','group_flatten']:body+=fn(name)
(out/'gl-body.inc').write_text(body)
fixture=ROOT/'scripts/tests/picasso-gl-groups-host.c';runs=[]
for name,flags in [('normal',[]),('asan',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
    binary=out/name
    for op,cmd in [('compile',['clang-18','-std=c11','-O1','-Wall','-Wextra','-Werror',*flags,'-I',str(out),str(fixture),'-Wl,-l:libGL.so.1','-Wl,-l:libEGL.so.1','-o',str(binary)]),('run',[str(binary)])]:
        env=dict(os.environ,LIBGL_ALWAYS_SOFTWARE='1',EGL_PLATFORM='surfaceless',ASAN_OPTIONS='detect_leaks=0')
        result=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=90)
        (out/(name+'-'+op+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stdout+result.stderr
        runs.append(dict(name=name+'-'+op,passed=True,output=result.stdout))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,
    source_sha256=sha(a.source),fixture_sha256=sha(fixture),runs=runs,
    inputs={str(p.relative_to(ROOT)):sha(p) for p in [fixture,Path(__file__)]},
    limitation='Production shaders compiled. Nonzero flatten pixels use an explicit surrogate to isolate GL state from llvmpipe framebuffer-fetch JIT; device wallpaper not tested.'),indent=2)+'\n')
print(runs[-1]['output'])
