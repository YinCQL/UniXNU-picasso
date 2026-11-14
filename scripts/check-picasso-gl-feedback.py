#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import subprocess,json,hashlib,re,os
import argparse
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description='Run production EAGL feedback/blur functions with Mesa software GL')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--declarations',type=Path,default=ROOT/'out/picasso/nativegl/eagl_gen.h')
a=p.parse_args();r=ROOT;e=ROOT/'scripts/tests';s=(r/'drivers/NativeGL/eagl/eagl.c').read_text()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
# Avoid prototypes/call-site matches by anchoring the actual function header.
def fn(name):
    m=re.search(r'static [^\n]+\n'+name+r'\(',s);assert m,name
    start=m.start();return s[start:s.index('\n}',start)+2]+'\n'
header=s[s.index('typedef unsigned int GLenum'):s.index('struct eagl;',s.index('typedef unsigned int GLenum'))]
for definition in re.findall(r'^#define GL_\w+\s+[^\n]+',s,re.M):
    if definition.split()[1] not in header:header+=definition+'\n'
header+='\n#define FUNCTION_VARIABLE_DOWNSAMPLE 38\n'
decl=a.declarations.read_text().split('#ifdef EAGL_GEN_DECLARATIONS\n')[1].split('#endif')[0]
chain=s[s.index('\tstruct vblur_chain {'):s.index('\n\tbool vblur_bound;')]
body=header+'\n'+decl+'''
struct eagl {unsigned program,colour,bound[8],unit,framebuffer,frames,clears,halve;GLint halve_texel;
GLbitfield unclear;GLboolean unclear_boxed,unclear_write[4];GLint unclear_box[4];GLfloat unclear_colour[4];
'''+chain+'''
};
static void say(const char *fmt,...){(void)fmt;}
static void nativegl_text(const char *s){puts(s);}
static void compositor_program(unsigned p,int good,const char *what){(void)p;(void)good;(void)what;}
static void clear_now(struct eagl *e);
static int eagl_tracing,compositor_diagnostics;
static void eagl_trace(const char *s,...){(void)s;}
#define EAGL_ENTER(e,o) ((void)(e),(void)(o))
#define EAGL_LEAVE(e,o) ((void)(e),(void)(o))
static void drawing(struct eagl *e){(void)e;}
struct group_texture {int state;};
#define GROUP_FRESH 1
static struct group_texture *group_of(struct eagl *e,GLuint t){(void)e;(void)t;return NULL;}
'''
body+=s[s.index('static const char vblur_vertex'):s.index('/*\n * Reading the chain:')]
for name in ['vblur_program','vblur_chain_of','vblur_level','resolve_feedback_clear','clear_now','my_glClear']:body+=fn(name)
(out/'gl-body.inc').write_text(body)
runs=[]
for name,flags in [('normal',[]),('asan',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
    binary=out/name
    for op,cmd in [('compile',['clang-18','-std=c11','-O1','-Wall','-Wextra','-Werror',*flags,'-I',str(out),str(e/'picasso-gl-feedback-host.c'),'-Wl,-l:libGL.so.1','-Wl,-l:libEGL.so.1','-o',str(binary)]),('run',[str(binary)])]:
        env=dict(os.environ,LIBGL_ALWAYS_SOFTWARE='1',EGL_PLATFORM='surfaceless',ASAN_OPTIONS='detect_leaks=0')
        p=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=90)
        (out/(name+'-'+op+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stdout+p.stderr
        runs.append(dict(name=name+'-'+op,passed=True,output=p.stdout))
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,source_sha256=hashlib.sha256(s.encode()).hexdigest(),fixture_sha256=hashlib.sha256((e/'picasso-gl-feedback-host.c').read_bytes()).hexdigest(),declarations_sha256=hashlib.sha256(a.declarations.read_bytes()).hexdigest(),runs=runs),indent=2)+'\n')
print(runs[-1]['output'])
