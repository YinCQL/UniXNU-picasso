#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run production IOSurface lifecycle functions with real Mesa GL and host surface memory."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=ROOT/'drivers/NativeGL/eagl/eagl.c')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--expect-stale-storage', action='store_true')
p.add_argument('--expect-unpublished-subimage', action='store_true')
p.add_argument('--expect-recycled-surface', action='store_true')
p.add_argument('--expect-stale-mipmap', action='store_true')
p.add_argument('--expect-stale-upload', action='store_true')
p.add_argument('--expect-stale-group', action='store_true')
p.add_argument('--expect-unpublished', action='store_true')
p.add_argument('--expect-unflipped', action='store_true')
p.add_argument('--expect-stale-read', action='store_true')
p.add_argument('--expect-shared-render', action='store_true')
p.add_argument('--expect-contaminated-upload', action='store_true')
a = p.parse_args()
s = a.source.read_text()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
def fn(name):
    m = re.search(r'static [^\n]+\n'+name+r'\(', s)
    assert m, name
    return s[m.start():s.index('\n}', m.start())+2]+'\n'
header = s[s.index('typedef unsigned int GLenum'):s.index('struct eagl;', s.index('typedef unsigned int GLenum'))]
for define in re.findall(r'^#define GL_\w+\s+[^\n]+', s, re.M):
    if define.split()[1] not in header: header += define+'\n'
decl = (ROOT/'out/picasso/nativegl/eagl_gen.h').read_text().split('#ifdef EAGL_GEN_DECLARATIONS\n')[1].split('#endif')[0]
metadata = s[s.index('#define SLOTS'):s.index('static unsigned trace_first')]
globals_ = s[s.index('static unsigned trace_first'):s.index('#define GL_SCISSOR_TEST')]
body = header+'\n'+decl+'\n'+metadata+'\n'+globals_+'''
static int eagl_tracing,frame_lines,latency_lines,compositor_diagnostics;
static unsigned long long slow_calls_us;
static void say(const char *fmt,...){(void)fmt;}
static unsigned refresh_note_count;
static char refresh_note_last[256];
static void ktrace(const char *fmt,...){
    if (!strncmp(fmt,"target-refresh ",15)) {
        va_list ap;va_start(ap,fmt);vsnprintf(refresh_note_last,sizeof(refresh_note_last),fmt,ap);va_end(ap);++refresh_note_count;
    }
}
static void eagl_trace(const char *fmt,...){(void)fmt;}
static void nativegl_text(const char *s){
    if (!strncmp(s,"target-refresh ",15)) {
        assert(strlen(s)<sizeof(refresh_note_last));strcpy(refresh_note_last,s);++refresh_note_count;
    }
}
static void nativegl_mark(unsigned v){(void)v;}
static uint64_t now_us(void){static uint64_t t;return ++t;}
static bool compositor_claim(unsigned b,unsigned k){(void)b;(void)k;return false;}
static unsigned w30r_notes;
static char w30r_last[256];
static void compositor_text(const char *fmt,...){
    if (!strncmp(fmt,"surface-w30r ",13)) {
        va_list ap;va_start(ap,fmt);vsnprintf(w30r_last,sizeof(w30r_last),fmt,ap);va_end(ap);++w30r_notes;
    }
}
static void compositor_framebuffer(struct eagl *e,const char *s){(void)e;(void)s;}
static void summary(struct eagl *e){(void)e;}
static void census_report(void){}
static void frame_functions(struct eagl *e){(void)e;}
static void my_glDeleteTextures(void *,GLsizei,const GLuint *);
static const char *census_name(unsigned f){(void)f;return "test";}
static const char *eagl_slot_name(unsigned o){(void)o;return "test";}
static void clear_now(struct eagl *e);
static void show_texture(struct eagl *e,struct image *i){(void)e;(void)i;}
static GLuint shown_texture;
static unsigned group_read_names[4],group_read_count;
static void group_read(struct eagl *e,GLuint t){(void)e;assert(group_read_count<4);group_read_names[group_read_count++]=t;}
static void vblur_forget(struct eagl *e,GLuint t){(void)e;(void)t;}
static void group_forget(struct eagl *e,GLuint t);
static struct group_texture *group_of(struct eagl *e,GLuint t){(void)e;(void)t;return NULL;}
static struct eagl *context_of(id o){return o;}
static void eagl_enter(struct eagl *e,unsigned o){(void)e;(void)o;}
static void eagl_leave(struct eagl *e,unsigned o){(void)e;(void)o;}
static void note_format(const char *s,GLenum a,GLenum b,GLenum c,GLsizei w,GLsizei h){(void)s;(void)a;(void)b;(void)c;(void)w;(void)h;}
static bool hakios_gl_surface_texture(struct hakios_gl *g,unsigned s);
#define EAGL_ENTER(e,o) eagl_enter(e,o)
#define EAGL_LEAVE(e,o) eagl_leave(e,o)
'''
body += 'static bool image_publish(struct eagl *,struct image *,const char *);\nstatic bool surface_publish_aliases(struct eagl *,IOSurfaceRef);\n'
names = ['texture_definition_of','texture_defined','image_of','group_forget','image_new','image_forget','internal_format','bytes_a_pixel',
         'surface_mirrored','surface_shared']
if 'images_release(' in s: names += ['images_release']
if 'surface_render_copy(' in s: names += ['surface_render_copy']
if 'image_render_target(' in s: names += ['image_render_target']
if 'mipmaps_ready(' in s: names += ['mipmaps_ready']
names += ['surface_changed','surface_to_texture','mirror_to_surface','target_to_surface','image_refresh_note','image_needs_upload','frame_begin','frame_end','drawing',
          'image_publish','surface_publish_aliases','my_glDeleteTextures','my_glTexSubImage2D','my_glCopyTexSubImage2D','my_glBindTexture','my_glClear','clear_now','my_glFramebufferTexture2D','my_glBindFramebuffer','my_texImageIOSurface','my_attachImage']
if 'my_glGenerateMipmap(' in s: names += ['my_glGenerateMipmap']
names += ['my_glTexImage2D','my_glTexStorage2D']
if 'image_storage_begin(' in s: names += ['storage_error_save','my_glGetError','image_storage_begin','image_storage_end']
body += 'static bool image_storage_begin(struct eagl *,GLenum,GLint,GLuint *);\nstatic void image_storage_end(struct eagl *,GLuint);\n' if 'image_storage_begin(' in s else ''
for name in names: body += fn(name)
if 'framebuffer_read_texture(' in s:
    for name in ['framebuffer_read_texture','my_glCopyTexImage2D','my_glReadPixels']: body += fn(name)
else:
    generated = (ROOT/'out/picasso/nativegl/eagl_gen.h').read_text()
    for name, alias in [('slot_01b8','my_glCopyTexImage2D'),('slot_0770','my_glReadPixels')]:
        at=generated.index('static void\n'+name+'(')
        body += generated[at:generated.index('\n}',at)+2]+'\n'
        body += '#define '+alias+' '+name+'\n'
body += "\nstatic unsigned stale_group_materializations;\n"
body += "static void registration_flatten(struct eagl *e,GLuint t,struct group_texture *g){(void)e;(void)t;g->state=GROUP_NONE;++stale_group_materializations;}\n"
body += fn('group_read').replace('group_read(', 'registration_read(').replace('group_flatten(', 'registration_flatten(')
(out/'gl-body.inc').write_text(body)
fixture = ROOT/'scripts/tests/picasso-gl-surfaces-host.c'
features = ([] if 'image_storage_begin(' not in s else ['-DEAGL_STORAGE_OWNERSHIP']) + ([] if 'IOSurfaceIncrementUseCount' not in s else ['-DEAGL_POOL_USES']) + ([] if 'image->drawn' not in fn('my_glTexSubImage2D') else ['-DEAGL_SUBIMAGE_PUBLISH'])
runs = []
for name, flags in [('normal', []), ('asan', ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-sanitize-recover=all'])]:
    binary = out/name
    commands = [('compile', ['clang-18','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',
                 *flags,*features,*(['-DEXPECT_STALE_STORAGE'] if a.expect_stale_storage else []),*(['-DEXPECT_UNPUBLISHED_SUBIMAGE'] if a.expect_unpublished_subimage else []),*(['-DEXPECT_RECYCLED_SURFACE'] if a.expect_recycled_surface else []),*(['-DEXPECT_STALE_GROUP'] if a.expect_stale_group else []),*(['-DEXPECT_STALE_UPLOAD'] if a.expect_stale_upload else []),*(['-DEXPECT_CONTAMINATED_UPLOAD'] if a.expect_contaminated_upload else []),*(['-DEXPECT_SHARED_RENDER'] if a.expect_shared_render else []),*(['-DEXPECT_STALE_READ'] if a.expect_stale_read else []),*(['-DEXPECT_UNFLIPPED'] if a.expect_unflipped else []),*(['-DEXPECT_UNPUBLISHED'] if a.expect_unpublished else []),*(['-DEXPECT_STALE_MIPMAP'] if a.expect_stale_mipmap else []),'-I',str(out),'-I',str(ROOT/'drivers/NativeGL/eagl'),
                 str(fixture),'-Wl,-l:libGL.so.1','-Wl,-l:libEGL.so.1','-o',str(binary)]),('run',[str(binary)])]
    for step, cmd in commands:
        result = subprocess.run(cmd,capture_output=True,text=True,timeout=90,
            env=dict(os.environ,LIBGL_ALWAYS_SOFTWARE='1',EGL_PLATFORM='surfaceless',ASAN_OPTIONS='detect_leaks=0'))
        (out/(name+'-'+step+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode == 0, result.stdout+result.stderr
        runs.append(dict(name=name+'-'+step,passed=True,output=result.stdout))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,baseline=a.expect_unpublished,contaminated_upload_reproduced=a.expect_contaminated_upload,
    stale_storage_reproduced=a.expect_stale_storage,stale_upload_reproduced=a.expect_stale_upload,stale_group_reproduced=a.expect_stale_group,
    unpublished_subimage_reproduced=a.expect_unpublished_subimage,recycled_surface_reproduced=a.expect_recycled_surface,source_sha256=sha(a.source),inputs={str(p.relative_to(ROOT)):sha(p) for p in (fixture,Path(__file__))},runs=runs,
    limitation='Real Mesa GLES pixels; IOSurface allocation, locks and seeds are a host fixture. EGLImage sibling import and storage detachment use real Mesa; Group registration dispatch is production group_read with a materialization observer, not a shader pixel test. Native IOSurface backing and Adreno execution require device verification.'),indent=2)+'\n')
print(runs[-1]['output'])
