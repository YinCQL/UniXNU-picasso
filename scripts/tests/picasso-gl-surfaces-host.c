// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "surface_hash.h"
typedef void *id,*SEL;
typedef int BOOL;
#define YES 1
#define NO 0
#define bzero(p,n) memset(p,0,n)
#define kIOSurfaceLockReadOnly 1
struct host_surface {unsigned id,seed,format,planes,bpe;size_t width,height,row,size;unsigned char *bytes;};
typedef struct host_surface *IOSurfaceRef;
static void *CFRetain(void *p){return p;}
static void CFRelease(void *p){(void)p;}
#define GETTER(name,field) static size_t IOSurfaceGet##name(IOSurfaceRef s){return s->field;}
GETTER(Width,width) GETTER(Height,height) GETTER(BytesPerRow,row) GETTER(BytesPerElement,bpe)
GETTER(PlaneCount,planes) GETTER(AllocSize,size)
static unsigned IOSurfaceGetID(IOSurfaceRef s){return s->id;}
static unsigned IOSurfaceGetSeed(IOSurfaceRef s){return s->seed;}
static unsigned IOSurfaceGetPixelFormat(IOSurfaceRef s){return s->format;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s){return s->bytes;}
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,unsigned p){(void)p;return s->row;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,unsigned p){(void)p;return s->bytes;}
static int IOSurfaceLock(IOSurfaceRef s,unsigned f,void *v){(void)s;(void)f;(void)v;return 0;}
static int IOSurfaceUnlock(IOSurfaceRef s,unsigned f,void *v){(void)v;if(!f)s->seed++;return 0;}
#include "gl-body.inc"
#ifndef GL_PACK_SKIP_ROWS
#define GL_PACK_SKIP_ROWS 0x0d03
#define GL_PACK_SKIP_PIXELS 0x0d04
#define GL_PIXEL_PACK_BUFFER 0x88eb
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ed
#endif
extern void *eglGetProcAddress(const char *);
extern unsigned eglInitialize(void *,int *,int *),eglBindAPI(unsigned),eglChooseConfig(void *,const int *,void **,int,int *);
extern void *eglCreateContext(void *,void *,void *,const int *);
extern unsigned eglMakeCurrent(void *,void *,void *,void *);
static void context(void){
    void *(*platform)(unsigned,void *,const int *)=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");assert(platform);
    void *display=platform(0x31dd,NULL,NULL);int a,b,n;assert(eglInitialize(display,&a,&b));
    assert(eglBindAPI(0x30a0));const int attrs[]={0x3040,0x40,0x3033,1,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038};
    void *config;assert(eglChooseConfig(display,attrs,&config,1,&n)&&n);
    const int ctxattrs[]={0x3098,3,0x3038};void *ctx=eglCreateContext(display,config,NULL,ctxattrs);assert(ctx);
    assert(eglMakeCurrent(display,NULL,NULL,ctx));printf("renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
}
static void expect_state(unsigned name,GLint want){GLint v=-1;glGetIntegerv(name,&v);assert(v==want);}
static void test_surface(bool invert){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[4*24];memset(bytes,0xa5,sizeof bytes);
    for(unsigned y=0;y<4;y++)memset(bytes+y*24,0,16);
    struct host_surface s={17,0,'BGRA',0,4,4,4,24,sizeof bytes,bytes};
    GLuint tex,fb,other;glGenTextures(1,&tex);my_glBindTexture(e,GL_TEXTURE_2D,tex);
    assert(my_texImageIOSurface(e,NULL,&s,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,invert));
    assert(!glGetError());struct image *i=image_of(e,tex);assert(i&&!i->target);
    glGenFramebuffers(1,&fb);glGenFramebuffers(1,&other);my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    frame_end(e,false);assert(!i->target&&s.seed==0); // Binding alone must not publish.
    glViewport(0,0,4,4);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    glClearColor(0.25,0.5,0.75,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,4,1);glClearColor(1,0,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);unsigned char pixel[4];
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(!glGetError());
    assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
    // Rebinding after a CPU seed change must not overwrite a GPU-owned target.
    s.seed++;my_glBindTexture(e,GL_TEXTURE_2D,tex);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,other);
#ifndef EXPECT_UNPUBLISHED
    GLuint buffer;glGenBuffers(1,&buffer);glBindBuffer(GL_PIXEL_PACK_BUFFER,buffer);glBufferData(GL_PIXEL_PACK_BUFFER,512,NULL,0x88e8);
    glPixelStorei(GL_PACK_ROW_LENGTH,7);glPixelStorei(GL_PACK_ALIGNMENT,8);glPixelStorei(GL_PACK_SKIP_ROWS,2);glPixelStorei(GL_PACK_SKIP_PIXELS,1);
#endif
    frame_end(e,false);
#ifdef EXPECT_UNPUBLISHED
    assert(!i->target&&s.seed==1);
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<16;x++)assert(bytes[y*24+x]==0);
    puts("REPRODUCED: GPU drawing through texImageIOSurface leaves shared surface black");
#else
    assert(i->target&&i->painted&&!i->drawn&&s.seed==2);
    expect_state(GL_READ_FRAMEBUFFER_BINDING,other);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,fb);
    expect_state(GL_PIXEL_PACK_BUFFER_BINDING,buffer);expect_state(GL_PACK_ROW_LENGTH,7);expect_state(GL_PACK_ALIGNMENT,8);
    expect_state(GL_PACK_SKIP_ROWS,2);expect_state(GL_PACK_SKIP_PIXELS,1);
    for(unsigned y=0;y<4;y++){
        unsigned gy=invert?3-y:y;
        for(unsigned x=0;x<4;x++){
            unsigned char *p=bytes+y*24+x*4;
            assert(p[0]==(gy?191:0)&&p[1]==(gy?128:0)&&p[2]==(gy?64:255)&&p[3]==255);
        }
        for(unsigned x=16;x<24;x++)assert(bytes[y*24+x]==0xa5);
    }
    frame_end(e,false);assert(s.seed==2); // No repeated publication without another write.
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glDeleteBuffers(1,&buffer);
    glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_ALIGNMENT,4);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
#endif
    assert(!glGetError());image_forget(e,tex);glDeleteTextures(1,&tex);glDeleteFramebuffers(1,&fb);glDeleteFramebuffers(1,&other);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);free(e);
}
#ifndef EXPECT_UNPUBLISHED
static void test_copy(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);unsigned char bytes[64]={0};
    struct host_surface s={19,0,'BGRA',0,4,4,4,16,64,bytes};
    GLuint source,dest,read,draw;glGenTextures(1,&source);glBindTexture(GL_TEXTURE_2D,source);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&read);my_glBindFramebuffer(e,GL_FRAMEBUFFER,read);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
    glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glGenTextures(1,&dest);my_glBindTexture(e,GL_TEXTURE_2D,dest);
    assert(my_texImageIOSurface(e,NULL,&s,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    glGenFramebuffers(1,&draw);my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,draw);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dest,0);
    struct group_texture group={0};e->groups=&group;group_read_count=0;
    my_glCopyTexSubImage2D(e,GL_TEXTURE_2D,0,0,0,0,0,4,4);
    assert(group_read_count==2&&group_read_names[0]==source&&group_read_names[1]==dest);
    frame_end(e,false);assert(s.seed==1);
    for(unsigned n=0;n<16;n++)assert(bytes[n*4]==0&&bytes[n*4+1]==255&&bytes[n*4+2]==0&&bytes[n*4+3]==255);
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,draw);
    assert(!glGetError());image_forget(e,dest);glDeleteTextures(1,&source);glDeleteTextures(1,&dest);
    glDeleteFramebuffers(1,&read);glDeleteFramebuffers(1,&draw);glDeleteFramebuffers(1,&e->reader);free(e);
}
static void test_rejections(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);unsigned char bytes[128]={0};
    struct host_surface s={18,0,'BGRA',0,4,4,4,16,64,bytes};
    GLuint tex;glGenTextures(1,&tex);my_glBindTexture(e,GL_TEXTURE_2D,tex);
    assert(my_texImageIOSurface(e,NULL,&s,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    struct image *i=image_of(e,tex);assert(i&&!i->target);
    frame_begin(e);frame_end(e,false);assert(!i->target&&s.seed==0); // Sampling remains read-only.
    s.planes=1;assert(!image_render_target(e,i));s.planes=0;
    s.bpe=8;assert(!image_render_target(e,i));s.bpe=4;
    s.format=0;assert(!image_render_target(e,i));s.format='BGRA';
    s.size=63;assert(!image_render_target(e,i));s.size=64;
    i->width=3;assert(!image_render_target(e,i));i->width=4;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);assert(!image_render_target(e,i));
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    GLuint fb;glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,1);
    drawing(e);assert(!i->target&&!i->drawn); // A mip-level write is not a full-surface write.
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(!glGetError());image_forget(e,tex);glDeleteTextures(1,&tex);glDeleteFramebuffers(1,&fb);free(e);
}
#endif
static void test_mirror(bool invert,bool damage){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint src,dst,fb,other;
    unsigned char pixels[64];for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        unsigned char *p=pixels+(y*4+x)*4;p[0]=y?0:255;p[1]=y?255:0;p[2]=0;p[3]=255;
    }
    glGenTextures(1,&src);glBindTexture(GL_TEXTURE_2D,src);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    memset(pixels,0,sizeof(pixels));glGenTextures(1,&dst);glBindTexture(GL_TEXTURE_2D,dst);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glGenFramebuffers(1,&fb);glGenFramebuffers(1,&other);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,other);
    glEnable(GL_SCISSOR_TEST);glScissor(7,7,1,1);struct image i={.texture=src,.mirror=dst,.width=4,.height=4,.invert=invert,.drawn=true};
    damage_blits=damage;if(damage){i.damaged=true;i.dmg[2]=4;i.dmg[3]=2;}
    assert(mirror_to_surface(e,&i));glFinish();
    expect_state(GL_READ_FRAMEBUFFER_BINDING,other);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,fb);assert(glIsEnabled(GL_SCISSOR_TEST));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,e->writer);glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        const unsigned char *p=pixels+(y*4+x)*4;unsigned gy=invert?3-y:y;
#ifdef EXPECT_UNFLIPPED
        if(invert)gy=damage?y-2:y;
#endif
        bool copied=!damage||(invert?y>=2:y<2);
        assert(p[0]==(copied&&gy==0?255:0)&&p[1]==(copied&&gy!=0?255:0)&&p[2]==0&&p[3]==(copied?255:0));
    }
    glDisable(GL_SCISSOR_TEST);damage_blits=0;glDeleteTextures(1,&src);glDeleteTextures(1,&dst);
    glDeleteFramebuffers(1,&fb);glDeleteFramebuffers(1,&other);glDeleteFramebuffers(1,&e->reader);glDeleteFramebuffers(1,&e->writer);free(e);
}
int main(void){context();
#ifdef EXPECT_UNFLIPPED
    test_mirror(true,false);test_mirror(true,true);puts("REPRODUCED: inverted GPU mirror keeps source rows in ascending order");return 0;
#endif
    test_surface(false);test_surface(true);
#ifndef EXPECT_UNPUBLISHED
    struct eagl *late=calloc(1,sizeof(*late));assert(late);late->frames=700;check_errors=0;
    glEnable(0xffffffffu);frame_end(late,false);assert(late->errors==1&&!glGetError());free(late);check_errors=1;
    test_mirror(false,false);test_mirror(true,false);test_mirror(true,true);test_copy();test_rejections();puts("PASS surface pixels, orientation/padding, seed ownership, lazy promotion, copy from independent READ framebuffer, layout/level rejection and GL pack/framebuffer state");
#endif
}
