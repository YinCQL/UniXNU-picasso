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
static IOSurfaceRef tracked_surface;
static int tracked_refs,tracked_uses,tracked_min_uses;
static void *CFRetain(void *p){if(p==tracked_surface)tracked_refs++;return p;}
static void CFRelease(void *p){if(p==tracked_surface){assert(tracked_refs>0);tracked_refs--;}}
static void IOSurfaceIncrementUseCount(IOSurfaceRef s){if(s==tracked_surface)tracked_uses++;}
static void IOSurfaceDecrementUseCount(IOSurfaceRef s){
    if(s==tracked_surface){assert(tracked_uses>0);tracked_uses--;if(tracked_uses<tracked_min_uses)tracked_min_uses=tracked_uses;}
}
static bool host_pool_recycle(IOSurfaceRef s){
    assert(s==tracked_surface);
    if(tracked_uses)return false;
    memset(s->bytes,255,s->size);s->seed++;return true;
}
#define GETTER(name,field) static size_t IOSurfaceGet##name(IOSurfaceRef s){return s->field;}
GETTER(Width,width) GETTER(Height,height) GETTER(BytesPerRow,row) GETTER(BytesPerElement,bpe)
GETTER(PlaneCount,planes) GETTER(AllocSize,size)
static unsigned IOSurfaceGetID(IOSurfaceRef s){return s->id;}
static unsigned IOSurfaceGetSeed(IOSurfaceRef s){return s->seed;}
static unsigned IOSurfaceGetPixelFormat(IOSurfaceRef s){return s->format;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s){return s->bytes;}
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,unsigned p){(void)p;return s->row;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,unsigned p){(void)p;return s->bytes;}
static int host_lock_error,host_unlock_error;
static bool host_race_after_unlock;
static bool host_replace_after_read;
static int IOSurfaceLock(IOSurfaceRef s,unsigned f,void *v){(void)f;if(v)*(uint32_t *)v=s->seed;return host_lock_error;}
static int IOSurfaceUnlock(IOSurfaceRef s,unsigned f,void *v){
    if(host_unlock_error)return host_unlock_error;
    if(!f)s->seed++;
    if(v)*(uint32_t *)v=s->seed;
    if(host_race_after_unlock)s->seed++;
    if(f && host_replace_after_read){
        for(size_t y=0;y<s->height;y++)for(size_t x=0;x<s->width;x++){
            unsigned char *p=s->bytes+y*s->row+x*4;p[0]=160;p[1]=80;p[2]=40;p[3]=255;
        }
        s->seed++;host_replace_after_read=false;
    }
    return 0;
}
#include "gl-body.inc"
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0cf3
#define GL_UNPACK_SKIP_PIXELS 0x0cf4
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
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
static void *egl_display;
static void *imported_image;
static void *(*create_image)(void *,void *,unsigned,void *,const int *);
static unsigned (*destroy_image)(void *,void *);
static void (*image_target)(unsigned,void *);
static bool hakios_gl_surface_texture(struct hakios_gl *g,unsigned sid){
    (void)g;
    if(sid!=201||!imported_image)return false;
    image_target(GL_TEXTURE_2D,imported_image);
    assert(!glGetError());return true;
}
static void context(void){
    void *(*platform)(unsigned,void *,const int *)=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");assert(platform);
    void *display=platform(0x31dd,NULL,NULL);int a,b,n;assert(eglInitialize(display,&a,&b));
    assert(eglBindAPI(0x30a0));const int attrs[]={0x3040,0x40,0x3033,1,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038};
    void *config;assert(eglChooseConfig(display,attrs,&config,1,&n)&&n);
    const int ctxattrs[]={0x3098,3,0x3038};void *ctx=eglCreateContext(display,config,NULL,ctxattrs);assert(ctx);
    assert(eglMakeCurrent(display,NULL,NULL,ctx));egl_display=display;
    create_image=(void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_image=(void *)eglGetProcAddress("eglDestroyImageKHR");
    image_target=(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    assert(create_image&&destroy_image&&image_target);
    printf("renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
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
    e->groups=calloc(GROUP_TEXTURES,sizeof(*e->groups));assert(e->groups);group_read_count=0;
    my_glCopyTexSubImage2D(e,GL_TEXTURE_2D,0,0,0,0,0,4,4);
    assert(group_read_count==2&&group_read_names[0]==source&&group_read_names[1]==dest);
    frame_end(e,false);assert(s.seed==1);
    for(unsigned n=0;n<16;n++)assert(bytes[n*4]==0&&bytes[n*4+1]==255&&bytes[n*4+2]==0&&bytes[n*4+3]==255);
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,draw);
    assert(!glGetError());image_forget(e,dest);glDeleteTextures(1,&source);glDeleteTextures(1,&dest);
    glDeleteFramebuffers(1,&read);glDeleteFramebuffers(1,&draw);glDeleteFramebuffers(1,&e->reader);free(e->groups);free(e);
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
static void test_independent_reads(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint source,target,read,draw;
    glGenTextures(1,&source);glBindTexture(GL_TEXTURE_2D,source);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&read);my_glBindFramebuffer(e,GL_FRAMEBUFFER,read);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
    glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    glGenTextures(1,&target);my_glBindTexture(e,GL_TEXTURE_2D,target);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&draw);my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,draw);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    e->groups=calloc(GROUP_TEXTURES,sizeof(*e->groups));assert(e->groups);group_read_count=0;unsigned char pixel[4];
    my_glReadPixels(e,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(group_read_count==1&&group_read_names[0]==source&&pixel[2]==255&&pixel[3]==255);
    my_glCopyTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,0,0,4,4,0);
    assert(group_read_count==2&&group_read_names[1]==source);
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,draw);
    assert(!glGetError());glDeleteTextures(1,&source);glDeleteTextures(1,&target);glDeleteFramebuffers(1,&read);glDeleteFramebuffers(1,&draw);free(e);
}
static void test_read_boundaries(bool copy){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint src,dst,fb;
    glGenTextures(1,&src);glBindTexture(GL_TEXTURE_2D,src);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,src,0);
    glViewport(0,0,4,4);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0,1,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);assert(e->unclear==GL_COLOR_BUFFER_BIT);
    glGenTextures(1,&dst);my_glBindTexture(e,GL_TEXTURE_2D,dst);
    unsigned char pixel[4];
    if(copy){
        my_glCopyTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,0,0,4,4,0);
        my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    }else my_glReadPixels(e,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(!glGetError());
#ifdef EXPECT_STALE_READ
    assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
#else
    assert(pixel[0]==0&&pixel[1]==255&&pixel[2]==0&&pixel[3]==255&&e->unclear==0);
    if(copy) assert(texture_definition_of(e,dst)==NULL); /* Default path retains no diagnostic history. */
#endif
    glDeleteTextures(1,&src);glDeleteTextures(1,&dst);glDeleteFramebuffers(1,&fb);free(e);
}
static void read_pixel(GLuint texture,unsigned char *pixel){
    GLuint fb;GLint read;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glReadPixels(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glDeleteFramebuffers(1,&fb);assert(!glGetError());
}
static void test_shared_render(bool mirror,bool copy){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint owner,texture,draw,read,other,source=0;
    unsigned char bytes[64]={0},pixel[4];
    struct host_surface surface={201,0,'BGRA',0,4,4,4,16,64,bytes};
    glGenTextures(1,&owner);glBindTexture(GL_TEXTURE_2D,owner);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&draw);glBindFramebuffer(GL_FRAMEBUFFER,draw);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,owner,0);
    glViewport(0,0,4,4);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    // The host import uses a real EGLImage sibling, not a successful import stub.
    extern void *eglGetCurrentContext(void);
    const int attributes[]={0x30d2,1,0x3038};
    imported_image=create_image(egl_display,eglGetCurrentContext(),0x30b1,(void *)(uintptr_t)owner,attributes);
    assert(imported_image);
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    struct image *i=image_of(e,texture);assert(i&&i->shared&&!i->target);read_pixel(texture,pixel);
    assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
    my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,draw);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    // GPU contents differ from the CPU fixture. Conversion must preserve the GPU copy.
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,1);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glGenTextures(1,&other);glGenFramebuffers(1,&read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,0x812f);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,9);glPixelStorei(GL_UNPACK_ALIGNMENT,8);
    GLuint unpack;glGenBuffers(1,&unpack);glBindBuffer(0x88ec,unpack);glBufferData(0x88ec,128,NULL,0x88e8);
    if(copy){
        glGenTextures(1,&source);glBindTexture(GL_TEXTURE_2D,source);
        glBindBuffer(0x88ec,0);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,read);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
        glDisable(GL_SCISSOR_TEST);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBindTexture(GL_TEXTURE_2D,texture);
        glEnable(GL_SCISSOR_TEST);glScissor(10,10,1,1);
        glBindBuffer(0x88ec,unpack);glPixelStorei(GL_UNPACK_ROW_LENGTH,9);glPixelStorei(GL_UNPACK_ALIGNMENT,8);
    }else{
        my_glBindTexture(e,GL_TEXTURE_2D,other); // Conversion must also preserve an unrelated binding.
        glScissor(1,1,2,2);glClearColor(0,1,0,1);
    }
    share_surfaces=mirror;damage_blits=1;
#ifndef EXPECT_SHARED_RENDER
    glEnable(0xffffffffu);assert(!image_render_target(e,i));
    assert(i->shared&&!i->target&&!i->mirror&&e->errors==1&&!glGetError());
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,draw);
    expect_state(GL_TEXTURE_BINDING_2D,copy?texture:other);
#endif
    if(copy)my_glCopyTexSubImage2D(e,GL_TEXTURE_2D,0,1,1,0,0,2,2);
    else {my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);}
    glFinish();assert(i->target);expect_state(GL_READ_FRAMEBUFFER_BINDING,read);
    expect_state(GL_DRAW_FRAMEBUFFER_BINDING,draw);expect_state(GL_TEXTURE_BINDING_2D,copy?texture:other);
    expect_state(GL_UNPACK_ROW_LENGTH,9);expect_state(GL_UNPACK_ALIGNMENT,8);expect_state(0x88ef,unpack);
    assert(glIsEnabled(GL_SCISSOR_TEST));
    glBindTexture(GL_TEXTURE_2D,texture);GLint parameter;
    glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&parameter);assert(parameter==GL_NEAREST);
    glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&parameter);assert(parameter==0x812f);
    read_pixel(texture,pixel);assert(pixel[0]==0&&pixel[1]==255&&pixel[2]==0&&pixel[3]==255);
    read_pixel(owner,pixel);
#ifdef EXPECT_SHARED_RENDER
    assert(i->shared&&!i->mirror&&pixel[0]==0&&pixel[1]==255);
    puts("REPRODUCED: promoted render texture still aliases imported EGLImage backing");
#else
    assert(!i->shared&&!!i->mirror==mirror&&pixel[0]==255&&pixel[1]==0);
    assert(texture_definition_of(e,texture)==NULL);
    assert(surface.seed==0);frame_end(e,false);assert(surface.seed==1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,draw);unsigned char actual[64];
    glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        bool green=x>=1&&x<3&&y>=1&&y<3,blue=x==0&&y==0;
        unsigned char *p=actual+(y*4+x)*4;
        assert(p[0]==(!green&&!blue?255:0)&&p[1]==(green?255:0)&&p[2]==(blue?255:0)&&p[3]==255);
    }
    if(mirror){read_pixel(owner,pixel);assert(pixel[0]==0&&pixel[1]==255&&pixel[3]==255);}
    else for(unsigned n=0;n<16;n++)assert(bytes[n*4]==actual[n*4+2]&&bytes[n*4+1]==actual[n*4+1]&&bytes[n*4+2]==actual[n*4]&&bytes[n*4+3]==255);
    frame_end(e,false);assert(surface.seed==1);
#endif
    glBindBuffer(0x88ec,0);glDeleteBuffers(1,&unpack);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glDisable(GL_SCISSOR_TEST);share_surfaces=1;damage_blits=0;image_forget(e,texture);
    assert(destroy_image(egl_display,imported_image));imported_image=NULL;
    glDeleteTextures(1,&owner);glDeleteTextures(1,&texture);glDeleteTextures(1,&other);if(source)glDeleteTextures(1,&source);
    glDeleteFramebuffers(1,&draw);glDeleteFramebuffers(1,&read);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);if(e->writer)glDeleteFramebuffers(1,&e->writer);
    assert(!glGetError());free(e);
}
static void test_shared_one_pixel(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint owner,texture,fb;unsigned char bytes[4]={0},pixel[4];
    struct host_surface surface={201,0,'BGRA',0,4,1,1,4,4,bytes};
    glGenTextures(1,&owner);glBindTexture(GL_TEXTURE_2D,owner);
    const unsigned char red[]={255,0,0,255};glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,red);
    extern void *eglGetCurrentContext(void);const int attributes[]={0x30d2,1,0x3038};
    imported_image=create_image(egl_display,eglGetCurrentContext(),0x30b1,(void *)(uintptr_t)owner,attributes);assert(imported_image);
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,1,1,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glViewport(0,0,1,1);glDisable(GL_SCISSOR_TEST);glClearColor(0,1,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    struct image *i=image_of(e,texture);assert(i->target&&!i->shared&&i->mirror);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,owner,0);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==255&&pixel[1]==0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    frame_end(e,false);assert(surface.seed==1);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,owner,0);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==0&&pixel[1]==255&&pixel[3]==255);
    image_forget(e,texture);assert(destroy_image(egl_display,imported_image));imported_image=NULL;
    glDeleteTextures(1,&owner);glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fb);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);if(e->writer)glDeleteFramebuffers(1,&e->writer);free(e);assert(!glGetError());
}

static void test_cpu_upload(bool buffer_bound, bool invert, bool alpha_first, bool planar, GLint skip_rows){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[216]={0},actual[64],expected[64];
    struct host_surface surface={301,0,'BGRA',planar,4,4,4,24,96,bytes};
    GLuint texture,fb,buffer=0;
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        unsigned char *p=bytes+y*24+x*4;
        p[0]=31+y;p[1]=63+x;p[2]=127+y+x;p[3]=255;
    }
    if(buffer_bound){
        glGenBuffers(1,&buffer);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER,256,NULL,0x88e8);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH,9);glPixelStorei(GL_UNPACK_ALIGNMENT,8);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,skip_rows);glPixelStorei(GL_UNPACK_SKIP_PIXELS,1);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,invert));
    struct image *i=image_of(e,texture);assert(i&&!i->shared&&!i->target);
    GLenum error=glGetError();
#ifdef EXPECT_CONTAMINATED_UPLOAD
    (void)alpha_first;
    assert(error==(buffer_bound?0x0502:0));
#else
    assert(!error);
#endif
    expect_state(GL_UNPACK_ROW_LENGTH,9);expect_state(GL_UNPACK_ALIGNMENT,8);
    expect_state(GL_UNPACK_SKIP_ROWS,skip_rows);expect_state(GL_UNPACK_SKIP_PIXELS,1);expect_state(GL_PIXEL_UNPACK_BUFFER_BINDING,buffer);
    glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,actual);assert(!glGetError());
#ifdef EXPECT_CONTAMINATED_UPLOAD
    for(unsigned n=0;n<16;n++){
        assert(actual[n*4]==(buffer_bound?255:0)&&actual[n*4+1]==0&&actual[n*4+2]==0&&actual[n*4+3]==(buffer_bound?255:0));
    }
    puts(buffer_bound?"REPRODUCED: IOSurface pointer interpreted as unpack-buffer offset; old texture retained":"REPRODUCED: inherited unpack skips upload black padding instead of surface pixels");
#else
    for(unsigned pass=0;pass<2;pass++){
        if(pass){
            // A later bind refreshes a CPU surface under the same caller state.
            alpha_first=alpha_first&&!planar;i->alpha_first=alpha_first;
            for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
                unsigned char *p=bytes+y*24+x*4;
                p[0]=alpha_first?255:47+y;p[1]=alpha_first?47+y:79+x;
                p[2]=alpha_first?79+x:143+y+x;p[3]=alpha_first?143+y+x:255;
            }
            surface.seed++;e->frames++;my_glBindTexture(e,GL_TEXTURE_2D,texture);
            assert(!glGetError()&&i->seed==surface.seed&&e->uploads==2);
            glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,actual);assert(!glGetError());
        }
        for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
            unsigned sy=invert?3-y:y;unsigned char *p=expected+(y*4+x)*4;
            p[0]=(pass?143:127)+sy+x;p[1]=(pass?79:63)+x;p[2]=(pass?47:31)+sy;p[3]=255;
        }
        assert(!memcmp(actual,expected,sizeof actual));
        expect_state(GL_UNPACK_ROW_LENGTH,9);expect_state(GL_UNPACK_ALIGNMENT,8);
        expect_state(GL_UNPACK_SKIP_ROWS,skip_rows);expect_state(GL_UNPACK_SKIP_PIXELS,1);expect_state(GL_PIXEL_UNPACK_BUFFER_BINDING,buffer);
    }
#endif
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);if(buffer)glDeleteBuffers(1,&buffer);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    image_forget(e,texture);glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fb);free(e);assert(!glGetError());
}
/* An application can write CPU pixels without moving the IOSurface seed.
 * The change detector must refresh those bytes, including the last pixel. */
static void test_unseeded_refresh(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[12]={0},pixels[12];
    struct host_surface surface={302,7,'BGRA',0,4,3,1,12,12,bytes};
    GLuint texture,fb;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,3,1,GL_BGRA,GL_UNSIGNED_BYTE,0,false));
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    bytes[8]=47;bytes[9]=83;bytes[10]=149;bytes[11]=255;
    e->frames++;my_glBindTexture(e,GL_TEXTURE_2D,texture);
    glReadPixels(0,0,3,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    assert(!glGetError()&&surface.seed==7&&e->uploads==2);
    assert(pixels[8]==149&&pixels[9]==83&&pixels[10]==47&&pixels[11]==255);
    image_forget(e,texture);glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fb);free(e);
}
static void test_snapshot_attachment(bool invert){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    const unsigned width=270,height=600,row=1088;size_t size=row*height;
    unsigned char *bytes=calloc(1,size);assert(bytes);
    struct host_surface s={76,0,0x77333072,0,4,width,height,row,size,bytes};
    GLuint tex,fb;glGenTextures(1,&tex);my_glBindTexture(e,GL_TEXTURE_2D,tex);
    // The unsupported packed snapshot leaves no attachment and no publication.
    assert(!my_attachImage(e,NULL,GL_TEXTURE_2D,&s,invert)&&!image_of(e,tex));
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE);
    assert(s.seed==0&&!memcmp(bytes,(unsigned char[16]){0},16));
    for(size_t n=0;n<size;n++)assert(bytes[n]==0);
    puts("REPRODUCED: w30r snapshot rejected by production attachImage; framebuffer incomplete, storage stays zero");
    // A surface actually allocated and advertised as BGRA uses the supported path.
    s.format='BGRA';
    for(unsigned y=0;y<height;y++)memset(bytes+y*row+width*4,0xa5,row-width*4);
    assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&s,invert));
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE&&!glGetError());
    glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    glClearColor(0.25,0.5,0.75,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,width,1);glClearColor(1,0,0,1);
    my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);glDisable(GL_SCISSOR_TEST);frame_end(e,false);
    assert(s.seed==1);
    for(unsigned y=0;y<height;y++){
        bool red=(invert?height-1-y:y)==0;
        for(unsigned x=0;x<width;x++){
            unsigned char *v=bytes+y*row+x*4;
            assert(v[0]==(red?0:191)&&v[1]==(red?0:128)&&v[2]==(red?255:64)&&v[3]==255);
        }
        for(unsigned x=width*4;x<row;x++)assert(bytes[y*row+x]==0xa5);
    }
    frame_end(e,false);assert(s.seed==1&&!glGetError());
    image_forget(e,tex);glDeleteTextures(1,&tex);glDeleteFramebuffers(1,&fb);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);free(bytes);free(e);
    puts("PASS: 270x600 BGRA snapshot attachment, colored GPU rendering and published pixels, orientation, padding and seed");
}

static GLuint sample_program(void) {
    const char *vs="#version 300 es\nprecision highp float;out vec2 uv;void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));uv=p;gl_Position=vec4(p*2.-1.,0.,1.);}";
    const char *fs="#version 300 es\nprecision highp float;uniform sampler2D t;in vec2 uv;out vec4 c;void main(){c=texture(t,uv);}";
    GLuint v=glCreateShader(GL_VERTEX_SHADER),f=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
    glShaderSource(v,1,&vs,NULL);glCompileShader(v);glShaderSource(f,1,&fs,NULL);glCompileShader(f);
    glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);
    glDeleteShader(v);glDeleteShader(f);return p;
}
static void scaled_pixel(struct eagl *e,GLuint input,GLuint fb,int scale,unsigned char *p) {
    my_glBindTexture(e,GL_TEXTURE_2D,input);
#ifndef EXPECT_STALE_MIPMAP
    mipmaps_ready(e);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER,fb);glViewport(0,0,scale,scale);
    glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(scale/2,scale/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(!glGetError());
}

static void test_mip_refresh(void) {
    struct eagl *e=calloc(1,sizeof(*e));assert(e);enum{W=64,ROW=W*4+16};
    unsigned char bytes[W*ROW];memset(bytes,0xa5,sizeof bytes);
    for(int y=0;y<W;y++)for(int x=0;x<W;x++){unsigned char *p=bytes+y*ROW+x*4;p[0]=p[1]=p[2]=0;p[3]=255;}
    struct host_surface surface={301,1,'BGRA',0,4,W,W,ROW,sizeof bytes,bytes};
    GLuint input,output,fb;glGenTextures(1,&input);my_glBindTexture(e,GL_TEXTURE_2D,input);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,W,W,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    glTexParameteri(GL_TEXTURE_2D,0x2801,0x2703);glTexParameteri(GL_TEXTURE_2D,0x2800,0x2601);
    glTexParameteri(GL_TEXTURE_2D,0x2802,0x812f);glTexParameteri(GL_TEXTURE_2D,0x2803,0x812f);
#ifdef EXPECT_STALE_MIPMAP
    glGenerateMipmap(GL_TEXTURE_2D);
#else
    my_glGenerateMipmap(e,GL_TEXTURE_2D);assert(image_of(e,input)->mipmapped);
#endif
    glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,W,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    GLuint program=sample_program();glUseProgram(program);unsigned char full[4],small[4];
    for(int pass=0;pass<4;pass++) {
        for(int y=0;y<W;y++)for(int x=0;x<W;x++){unsigned char *p=bytes+y*ROW+x*4;p[0]=160;p[1]=80;p[2]=40+pass*10;p[3]=255;}
        e->frames+=16; // Include an unannounced CPU write as well as a seed change.
        if(pass!=1)surface.seed++;
        if(pass==2||pass==3) {
            if(pass==3)surface.id++; // The same texture can move to another surface.
            my_glBindTexture(e,GL_TEXTURE_2D,input);
            assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,W,W,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
        }
        scaled_pixel(e,input,fb,W,full);scaled_pixel(e,input,fb,W/4,small);
        assert(full[0]==40+pass*10&&full[1]==80&&full[2]==160&&full[3]==255);
#ifdef EXPECT_STALE_MIPMAP
        assert(!small[0]&&!small[1]&&!small[2]&&small[3]==255);
        if(pass==0){puts("REPRODUCED production refresh: full-size content is colored; scaled content remains black");break;}
#else
        assert(!memcmp(full,small,4));
        unsigned char edge[W*4];glReadPixels(0,0,1,W/4,GL_RGBA,GL_UNSIGNED_BYTE,edge);
        for(int y=0;y<W/4;y++)assert(!memcmp(edge+y*4,full,4));
        GLint filter;glGetTexParameteriv(GL_TEXTURE_2D,0x2801,&filter);assert(filter==0x2703);
        for(int y=0;y<W;y++)for(int at=W*4;at<ROW;at++)assert(bytes[y*ROW+at]==0xa5);
#endif
    }
#ifndef EXPECT_STALE_MIPMAP
    my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,input,0);
    glUseProgram(0);glClearColor(.75,.5,.25,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    assert(image_of(e,input)->mip_pending&&e->mip_pending);
    frame_end(e,false);
    my_glFramebufferTexture2D(e,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);
    glUseProgram(program);scaled_pixel(e,input,fb,W/4,small);
    assert(small[0]==191&&small[1]==128&&small[2]==64&&small[3]==255&&!image_of(e,input)->mip_pending);
#endif
    glUseProgram(0);glDeleteProgram(program);image_forget(e,input);glDeleteTextures(1,&input);glDeleteTextures(1,&output);glDeleteFramebuffers(1,&fb);free(e);
#ifndef EXPECT_STALE_MIPMAP
    puts("PASS scaled surface pixels and left edge: seed/hash refresh, repeated registration, replacement surface, padding, original filter");
#endif
}


#ifndef EXPECT_STALE_MIPMAP
static void test_shared_mip(void) {
    struct eagl *e=calloc(1,sizeof(*e));assert(e);GLuint owner,texture,output,fb;
    unsigned char bytes[64*64*4]={0};struct host_surface surface={201,0,'BGRA',0,4,64,64,256,sizeof bytes,bytes};
    glGenTextures(1,&owner);glBindTexture(GL_TEXTURE_2D,owner);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,bytes);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,owner,0);
    glViewport(0,0,64,64);glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
    extern void *eglGetCurrentContext(void);const int attributes[]={0x30d2,1,0x3038};
    imported_image=create_image(egl_display,eglGetCurrentContext(),0x30b1,(void *)(uintptr_t)owner,attributes);assert(imported_image);
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,64,64,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(image_of(e,texture)->shared);
    glTexParameteri(GL_TEXTURE_2D,0x2801,0x2703);glTexParameteri(GL_TEXTURE_2D,0x2800,0x2601);
    glTexParameteri(GL_TEXTURE_2D,0x2802,0x812f);glTexParameteri(GL_TEXTURE_2D,0x2803,0x812f);
    my_glGenerateMipmap(e,GL_TEXTURE_2D);assert(!image_of(e,texture)->shared&&image_of(e,texture)->mipmapped&&!glGetError());
    glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);
    GLuint program=sample_program();glUseProgram(program);unsigned char full[4],small[4];
    scaled_pixel(e,texture,fb,64,full);scaled_pixel(e,texture,fb,16,small);
    // Initial CPU memory is zero: this must be the preserved GPU image.
    assert(full[0]==64&&full[1]==128&&full[2]==191&&full[3]==255&&!memcmp(full,small,4));
    for(int i=0;i<64*64;i++){bytes[i*4]=160;bytes[i*4+1]=80;bytes[i*4+2]=40;bytes[i*4+3]=255;}++surface.seed;
    scaled_pixel(e,texture,fb,64,full);scaled_pixel(e,texture,fb,16,small);
    assert(full[0]==40&&full[1]==80&&full[2]==160&&full[3]==255&&!memcmp(full,small,4));
    my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,64,64,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(!image_of(e,texture)->shared&&image_of(e,texture)->mipmapped);
    scaled_pixel(e,texture,fb,16,small);assert(!memcmp(full,small,4));
    glUseProgram(0);glDeleteProgram(program);image_forget(e,texture);glDeleteTextures(1,&texture);glDeleteTextures(1,&owner);glDeleteTextures(1,&output);glDeleteFramebuffers(1,&fb);
    assert(destroy_image(egl_display,imported_image));imported_image=NULL;free(e);
    puts("PASS shared mip generation: preserved GPU pixels, independent storage, refreshed levels, repeated import");
}
#endif

static void test_external_target_refresh(void) {
    for (unsigned start=0;start<3;start++) {
        struct eagl *e=calloc(1,sizeof(*e));assert(e);
        unsigned char bytes[8*40];memset(bytes,0,sizeof bytes);
        struct host_surface s={301+start,1,'BGRA',0,4,8,8,40,sizeof bytes,bytes};
        GLuint tex;glGenTextures(1,&tex);my_glBindTexture(e,GL_TEXTURE_2D,tex);
        assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&s,0));
        struct image *i=image_of(e,tex);assert(i&&i->target&&!i->shared&&!i->drawn);
        if(start==1) {
            for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
                unsigned char *p=bytes+y*40+x*4;
                p[0]=x?160:0;p[1]=x?80:0;p[2]=x?40:0;p[3]=255;
            }
            ++s.seed;surface_to_texture(e,i);
        }
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,0x2703);
        my_glGenerateMipmap(e,GL_TEXTURE_2D);
        GLuint fb;glGenFramebuffers(1,&fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
        if(start==2) {
            my_glBindFramebuffer(e,GL_DRAW_FRAMEBUFFER,fb);
            glViewport(0,0,8,8);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
            glClearColor(1,0,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
            assert(i->drawn);++s.seed;
            my_glBindTexture(e,GL_TEXTURE_2D,tex);frame_begin(e);
            unsigned char local[4];glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,local);
            assert(local[0]==255&&local[1]==0&&local[2]==0);
            uint32_t old_seed=i->seed;
            host_lock_error=1;target_to_surface(e,i);host_lock_error=0;
            assert(i->drawn&&i->seed==old_seed);
            host_unlock_error=1;target_to_surface(e,i);host_unlock_error=0;
            assert(i->drawn&&i->seed==old_seed);
            host_race_after_unlock=true;target_to_surface(e,i);host_race_after_unlock=false;
            assert(!i->drawn&&i->seed+1==s.seed);
            // Another producer changing seed immediately after unlock must be noticed.
            unsigned uploads=e->uploads;my_glBindTexture(e,GL_TEXTURE_2D,tex);
            assert(e->uploads==uploads+1&&i->seed==s.seed);
            // Shared publication has the same retry and returned-seed contract.
            i->shared=true;i->drawn=true;old_seed=i->seed;
            host_lock_error=1;target_to_surface(e,i);host_lock_error=0;
            assert(i->drawn&&i->seed==old_seed);
            host_unlock_error=1;target_to_surface(e,i);host_unlock_error=0;
            assert(i->drawn&&i->seed==old_seed);
            host_race_after_unlock=true;target_to_surface(e,i);host_race_after_unlock=false;
            assert(!i->drawn&&i->seed+1==s.seed);i->shared=false;
            my_glBindTexture(e,GL_TEXTURE_2D,tex);frame_end(e,false);
            assert(i->seed==s.seed);
            uploads=e->uploads;
            my_glBindTexture(e,GL_TEXTURE_2D,tex);assert(e->uploads==uploads);
        }
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            unsigned char *p=bytes+y*40+x*4;p[0]=160;p[1]=80;p[2]=40;p[3]=255;
        }
        ++s.seed;
        if(start==0) frame_begin(e); else my_glBindTexture(e,GL_TEXTURE_2D,tex);
        assert(i->seed==s.seed);
        for(int level=0;level<3;level++) {
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,level);
            unsigned char pixels[8*8*4];int width=8>>level;
            glReadPixels(0,0,width,width,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
            for(int j=0;j<width*width;j++)assert(pixels[j*4]==40&&pixels[j*4+1]==80&&pixels[j*4+2]==160&&pixels[j*4+3]==255);
        }
        unsigned uploads=e->uploads;my_glBindTexture(e,GL_TEXTURE_2D,tex);assert(e->uploads==uploads);
        // Only external target updates emit records, with a strict context limit.
        unsigned notes=refresh_note_count;
        for(unsigned repeat=0;repeat<20;repeat++){++s.seed;my_glBindTexture(e,GL_TEXTURE_2D,tex);}
        assert(e->target_refresh_notes==8&&refresh_note_count-notes<=8);
        assert(strstr(refresh_note_last,"at=bind")&&strlen(refresh_note_last)<200);
        notes=refresh_note_count;uploads=e->uploads;
        i->painted=true;
        for(unsigned repeat=0;repeat<10000;repeat++)my_glBindTexture(e,GL_TEXTURE_2D,tex);
        assert(refresh_note_count==notes&&e->uploads==uploads);
        image_forget(e,tex);glDeleteTextures(1,&tex);glDeleteFramebuffers(1,&fb);free(e);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
    }
    puts("PASS externally updated target: seed refresh at bind/frame begin, full mip levels and left edge, pending local GPU preservation, own publish baseline and post-unlock race, failed publish retry, bounded notes, no unchanged upload");
}


static void test_surface_handoff(unsigned kind,bool invert,bool fail){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[6*40],replacement[6*40];memset(bytes,0xa5,sizeof bytes);memset(replacement,0x55,sizeof replacement);
    for(unsigned y=0;y<6;y++)memset(bytes+y*40,0,32);
    struct host_surface surface={517,0,'BGRA',0,4,8,6,40,sizeof bytes,bytes};
    struct host_surface next={518,0,'BGRA',0,4,8,6,40,sizeof replacement,replacement};
    GLuint texture,fb,alias=0;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,invert));
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glViewport(0,0,8,6);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    glClearColor(1,0,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,6);glClearColor(0,1,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);
    if(fail){
        host_lock_error=1;assert(!my_attachImage(e,NULL,GL_TEXTURE_2D,&next,invert));host_lock_error=0;
        assert(image_of(e,texture)->surface==&surface&&image_of(e,texture)->drawn&&surface.seed==0);
    }
    if(kind==0){
        host_lock_error=1;my_glDeleteTextures(e,1,&texture);host_lock_error=0;
        assert(image_of(e,texture)&&image_of(e,texture)->drawn&&glIsTexture(texture));
        if(invert)frame_end(e,false);else my_glDeleteTextures(e,1,&texture);
        assert(!image_of(e,texture)&&!glIsTexture(texture));
    }
    if(kind==1)assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,invert));
    if(kind==2)assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&next,invert));
    if(kind==3){
        glGenTextures(1,&alias);my_glBindTexture(e,GL_TEXTURE_2D,alias);
        assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,6,GL_BGRA,GL_UNSIGNED_BYTE,0,invert));
    }
    assert(surface.seed==1);
    for(unsigned y=0;y<6;y++){
        for(unsigned x=0;x<8;x++){
            unsigned char *v=bytes+y*40+x*4;assert(v[0]==0&&v[1]==(x==0?255:0)&&v[2]==(x==0?0:255)&&v[3]==255);
        }
        for(unsigned x=32;x<40;x++)assert(bytes[y*40+x]==0xa5);
    }
    if(kind==1||kind==3){
        GLuint shown=kind==1?texture:alias;unsigned char pixel[4];read_pixel(shown,pixel);
        assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
    }
    assert(e->surface_handoff_notes>=1&&e->surface_handoff_notes<=2);
    assert(!glGetError());glDisable(GL_SCISSOR_TEST);
    if(kind!=0){image_forget(e,texture);glDeleteTextures(1,&texture);}
    if(alias){image_forget(e,alias);glDeleteTextures(1,&alias);}
    glDeleteFramebuffers(1,&fb);if(e->reader)glDeleteFramebuffers(1,&e->reader);free(e);
}

static void test_mirror_handoff(bool replace, bool invert) {
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[64]={0},pixels[64];
    struct host_surface surface={201,0,'BGRA',0,4,4,4,16,64,bytes};
    GLuint owner,texture,fb,read,other;
    glGenTextures(1,&owner);glBindTexture(GL_TEXTURE_2D,owner);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,bytes);
    extern void *eglGetCurrentContext(void);const int attributes[]={0x30d2,1,0x3038};
    imported_image=create_image(egl_display,eglGetCurrentContext(),0x30b1,(void *)(uintptr_t)owner,attributes);assert(imported_image);
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,invert));
    struct image *i=image_of(e,texture);assert(i->mirror&&!i->shared);
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glViewport(0,0,4,4);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    glClearColor(1,0,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);clear_now(e);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,1);glClearColor(0,1,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);
    glGenFramebuffers(1,&read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,owner,0);
    glGenTextures(1,&other);my_glBindTexture(e,GL_TEXTURE_2D,other);
    damage_blits=1;host_lock_error=1;
    assert(!image_publish(e,i,"test"));host_lock_error=0;
    assert(i->drawn&&surface.seed==0);
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,fb);
    expect_state(GL_TEXTURE_BINDING_2D,other);assert(glIsEnabled(GL_SCISSOR_TEST));
    if(replace){
        my_glBindTexture(e,GL_TEXTURE_2D,texture);
        assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
        assert(image_of(e,texture)->shared);
    }else{
        my_glDeleteTextures(e,1,&texture);assert(!image_of(e,texture)&&!glIsTexture(texture));
        expect_state(GL_TEXTURE_BINDING_2D,other);
    }
    expect_state(GL_READ_FRAMEBUFFER_BINDING,read);expect_state(GL_DRAW_FRAMEBUFFER_BINDING,fb);
    assert(surface.seed==1&&glIsEnabled(GL_SCISSOR_TEST));
    glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        bool green=x==0&&y==(invert?3:0);unsigned char *v=pixels+4*(4*y+x);
        assert(v[0]==(green?0:255)&&v[1]==(green?255:0)&&v[2]==0&&v[3]==255);
    }
    // The fake CPU allocation is intentionally disjoint from the EGLImage.
    for(unsigned n=0;n<sizeof bytes;n++)assert(bytes[n]==0);
    if(replace){image_forget(e,texture);glDeleteTextures(1,&texture);}
    assert(destroy_image(egl_display,imported_image));imported_image=NULL;
    glDeleteTextures(1,&owner);glDeleteTextures(1,&other);glDeleteFramebuffers(1,&fb);glDeleteFramebuffers(1,&read);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);if(e->writer)glDeleteFramebuffers(1,&e->writer);
    glDisable(GL_SCISSOR_TEST);damage_blits=0;free(e);assert(!glGetError());
}

static void test_upload_seed_race(bool edge) {
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[8*40];memset(bytes,0xa5,sizeof bytes);
    struct host_surface surface={601,1,'BGRA',0,4,8,8,40,sizeof bytes,bytes};
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
        unsigned char *p=bytes+y*40+x*4;memset(p,255,4);
    }
    GLuint texture,fb;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,NO));
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(1,1,1,1);
    my_glClear(e,GL_COLOR_BUFFER_BIT);frame_end(e,false);
    struct image *i=image_of(e,texture);assert(i->painted&&!i->drawn);
    if(edge)for(unsigned y=0;y<8;y++)memset(bytes+y*40,0,3);
    surface.seed++;
    host_replace_after_read=true;my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(!host_replace_after_read);
    unsigned uploads=e->uploads;
    // Another producer publishes after the read lock ends. This upload contains
    // the older white image (or its black left column), never the new pixels.
    for(unsigned repeat=0;repeat<20;repeat++){e->in_frame=false;frame_begin(e);my_glBindTexture(e,GL_TEXTURE_2D,texture);}
    unsigned char pixels[8*8*4];glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
#ifdef EXPECT_STALE_UPLOAD
    assert(e->uploads==uploads&&i->seed==surface.seed);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
        unsigned char *p=pixels+(y*8+x)*4;unsigned v=edge&&x==0?0:255;
        assert(p[0]==v&&p[1]==v&&p[2]==v&&p[3]==255);
    }
#else
    assert(e->uploads==uploads+1&&i->seed==surface.seed);
    for(unsigned n=0;n<64;n++)assert(pixels[n*4]==40&&pixels[n*4+1]==80&&pixels[n*4+2]==160&&pixels[n*4+3]==255);
#endif
    for(unsigned y=0;y<8;y++)for(unsigned x=32;x<40;x++)assert(bytes[y*40+x]==0xa5);
    image_forget(e,texture);glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fb);
    if(e->reader)glDeleteFramebuffers(1,&e->reader);free(e);glBindFramebuffer(GL_FRAMEBUFFER,0);
}

static void test_registration_group(bool attach) {
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    e->groups=calloc(GROUP_TEXTURES,sizeof(*e->groups));assert(e->groups);
    unsigned char bytes[64];memset(bytes,255,sizeof bytes);
    struct host_surface surface={701,0,'BGRA',0,4,4,4,16,sizeof bytes,bytes};
    GLuint texture;glGenTextures(1,&texture);assert(texture<GROUP_TEXTURES);
    my_glBindTexture(e,GL_TEXTURE_2D,texture);
    struct group_texture *g=&e->groups[texture];g->state=GROUP_VIBRANT;g->colours[19]=1;
    // Rejected registration retains the old material's state.
    assert(!my_attachImage(e,NULL,0,&surface,NO)&&g->state==GROUP_VIBRANT);
    if(attach)assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,NO));
    else assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    unsigned before=stale_group_materializations;
    registration_read(e,texture);
#ifdef EXPECT_STALE_GROUP
    assert(stale_group_materializations==before+1);
#else
    assert(g->state==GROUP_NONE&&stale_group_materializations==before);
#endif
    // Legitimate ordinary groups still reach their materialization path.
    image_forget(e,texture);g->state=GROUP_VIBRANT;before=stale_group_materializations;
    registration_read(e,texture);assert(stale_group_materializations==before+1);
    glDeleteTextures(1,&texture);free(e->groups);free(e);
}

/* A CF reference keeps the object alive but does not reserve its pool slot.
 * After the producer returns to its pool, the consumer must keep the pixels. */
#if defined(EAGL_POOL_USES) || defined(EXPECT_RECYCLED_SURFACE)
static void test_pool_lifetime(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[8*40];memset(bytes,0xa5,sizeof bytes);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
        unsigned char *p=bytes+y*40+x*4;p[0]=160;p[1]=80;p[2]=40;p[3]=255;
    }
    struct host_surface surface={801,1,'BGRA',0,4,8,8,40,sizeof bytes,bytes};
    tracked_surface=&surface;tracked_refs=tracked_uses=0;tracked_min_uses=100;
    GLuint texture,alias,fb;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(tracked_refs==1);bool recycled=host_pool_recycle(&surface);
    e->frames++;my_glBindTexture(e,GL_TEXTURE_2D,texture);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    unsigned char pixels[8*8*4];glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
#ifdef EXPECT_RECYCLED_SURFACE
    assert(recycled&&tracked_uses==0);
    for(unsigned i=0;i<sizeof pixels;i++)assert(pixels[i]==255);
    puts("REPRODUCED: retained IOSurface recycled by pool; next texture refresh becomes all white");
#else
    assert(!recycled&&tracked_uses==1);
    for(unsigned i=0;i<sizeof pixels;i+=4)assert(pixels[i]==40&&pixels[i+1]==80&&pixels[i+2]==160&&pixels[i+3]==255);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(tracked_refs==1&&tracked_uses==1&&tracked_min_uses==1);
    glGenTextures(1,&alias);my_glBindTexture(e,GL_TEXTURE_2D,alias);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(tracked_refs==2&&tracked_uses==2);
    my_glDeleteTextures(e,1,&texture);assert(tracked_refs==1&&tracked_uses==1&&!host_pool_recycle(&surface));
    my_glDeleteTextures(e,1,&alias);assert(!tracked_refs&&!tracked_uses&&host_pool_recycle(&surface));
    // Teardown has to return all pool reservations, including undeleted names.
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    images_release(e);assert(!e->image_count&&!tracked_refs&&!tracked_uses);
    // Replacement failure retains the old pool reservation for a retry.
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_attachImage(e,NULL,GL_TEXTURE_2D,&surface,NO));
    struct image *im=image_of(e,texture);im->drawn=true;
    host_lock_error=1;assert(!image_new(e,texture,&surface));host_lock_error=0;
    assert(tracked_refs==1&&tracked_uses==1&&im->drawn);
    im->drawn=false;images_release(e);assert(!tracked_refs&&!tracked_uses);
    // The bounded image table returns the evicted pool reservation.
    e->image_count=sizeof(e->images)/sizeof(e->images[0]);
    memset(e->images,0,sizeof e->images);
    for(unsigned k=0;k<e->image_count;k++)e->images[k].target=true;
    e->images[0].target=false;e->images[0].texture=texture;e->images[0].surface=&surface;
    CFRetain(&surface);IOSurfaceIncrementUseCount(&surface);
    memset(e->bound,0,sizeof e->bound);
    // Alias publication reads identities for every live entry.
    for(unsigned k=1;k<e->image_count;k++)e->images[k].surface=&surface;
    tracked_min_uses=1;
    assert(image_new(e,texture+10000,&surface));
    assert(e->image_evicted==1&&tracked_refs==1&&tracked_uses==1&&tracked_min_uses==1);
    e->image_count=1;images_release(e);assert(!tracked_refs&&!tracked_uses);
    puts("PASS pool lifetime: pixels including left edge survive producer release; balanced alias/re-registration/delete/context cleanup/failure/eviction");
#endif
    glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&texture);free(e);tracked_surface=NULL;
}

#endif

/* Partial texture uploads also write an IOSurface view. A later snapshot
 * consumer and generated mip levels must both see the updated left column. */
static void test_subimage_publication(bool invert,bool buffered){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char bytes[8*40];memset(bytes,0xa5,sizeof bytes);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
        unsigned char *p=bytes+y*40+x*4;p[0]=p[1]=p[2]=x?255:0;p[3]=255;
    }
    struct host_surface surface={802,1,'BGRA',0,4,8,8,40,sizeof bytes,bytes};
    GLuint texture,fb;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,invert));
    my_glGenerateMipmap(e,GL_TEXTURE_2D);
    unsigned char column[8*4];memset(column,255,sizeof column);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    GLuint unpack=0;const void *input=column;
    if(buffered){
        unsigned char data[128];memset(data,0,sizeof data);memcpy(data+32,column,sizeof column);
        glGenBuffers(1,&unpack);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);
        glBufferData(GL_PIXEL_UNPACK_BUFFER,sizeof data,data,0x88e8);input=(void *)32;
    }
    glEnable(GL_SCISSOR_TEST);glScissor(3,3,1,1);
    my_glTexSubImage2D(e,GL_TEXTURE_2D,0,0,0,1,8,GL_RGBA,GL_UNSIGNED_BYTE,input);
    expect_state(GL_PIXEL_UNPACK_BUFFER_BINDING,(GLint)unpack);
    assert(glIsEnabled(GL_SCISSOR_TEST));glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);if(unpack)glDeleteBuffers(1,&unpack);
    assert(!glGetError());
    frame_end(e,false);mipmaps_ready(e);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,1);
    unsigned char pixel[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(!glGetError());
#ifdef EXPECT_UNPUBLISHED_SUBIMAGE
    assert(surface.seed==1&&bytes[0]==0&&pixel[0]<200);
    puts("REPRODUCED: partial upload leaves persisted left column black and generated mip level stale");
#else
    assert(surface.seed==2&&pixel[0]==255&&pixel[1]==255&&pixel[2]==255&&pixel[3]==255);
    for(unsigned y=0;y<8;y++){
        for(unsigned x=0;x<8*4;x++)assert(bytes[y*40+x]==255);
        for(unsigned x=8*4;x<40;x++)assert(bytes[y*40+x]==0xa5);
    }
    puts("PASS partial upload publishes corrected left edge and refreshes generated mip levels without touching row padding");
#endif
#ifndef EXPECT_UNPUBLISHED_SUBIMAGE
    // An invalid upload and a manually supplied mip level do not publish level zero.
    struct image *im=image_of(e,texture);assert(!im->drawn);
    my_glTexSubImage2D(e,GL_TEXTURE_2D,0,-1,0,1,8,GL_RGBA,GL_UNSIGNED_BYTE,column);
    assert(glGetError()!=0&&!im->drawn);frame_end(e,false);assert(surface.seed==2);
    my_glTexSubImage2D(e,GL_TEXTURE_2D,1,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,column);
    assert(!glGetError()&&!im->drawn);frame_end(e,false);assert(surface.seed==2);
#endif
    my_glDeleteTextures(e,1,&texture);glDeleteFramebuffers(1,&fb);free(e);
}


static void test_storage_replacement(unsigned method,bool black_edge){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);
    unsigned char backing[8*40],replacement[8*8*4];memset(backing,255,sizeof backing);
    if(black_edge)for(unsigned y=0;y<8;y++){unsigned char *v=backing+y*40;v[0]=v[1]=v[2]=0;}
    for(unsigned n=0;n<64;n++){replacement[4*n]=40;replacement[4*n+1]=80;replacement[4*n+2]=160;replacement[4*n+3]=255;}
    struct host_surface surface={903,1,'BGRA',0,4,8,8,40,sizeof backing,backing};
    GLuint texture,fb,source=0;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,8,8,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
    if(method==0)my_glTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    else if(method==1){
        glGenTextures(1,&source);glBindTexture(GL_TEXTURE_2D,source);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
        glBindTexture(GL_TEXTURE_2D,texture);my_glCopyTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,0,0,8,8,0);
    }else{
        my_glTexStorage2D(e,GL_TEXTURE_2D,1,0x8058,8,8);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    }
    assert(!glGetError());glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    unsigned char pixel[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==40);
    surface.seed++; // The former surface producer publishes while this name now has ordinary storage.
    frame_end(e,false);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
#ifdef EXPECT_STALE_STORAGE
    if(method!=2){assert(pixel[0]==(black_edge?0:255));puts("REPRODUCED former IOSurface refresh overwrites replacement texture with white or black edge");}
    else {assert(glGetError()!=0);puts("REPRODUCED former IOSurface refresh attempts illegal write into immutable storage");}
#else
    assert(!glGetError()&&pixel[0]==40&&pixel[1]==80&&pixel[2]==160&&pixel[3]==255);
    assert(image_of(e,texture)==NULL);puts("PASS storage replacement cannot refresh from its retired IOSurface");
#endif
    image_forget(e,texture);glDeleteTextures(1,&texture);if(source)glDeleteTextures(1,&source);
    glDeleteFramebuffers(1,&fb);free(e);
}


#ifdef EAGL_STORAGE_OWNERSHIP
static void test_storage_failures_and_publish(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);unsigned char bytes[64]={0},fresh[64];memset(fresh,255,sizeof fresh);
    struct host_surface surface={904,1,'BGRA',0,4,4,4,16,64,bytes};
    GLuint texture,fb;glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    int errors=check_errors;check_errors=0;
    my_glTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,-1,4,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    assert(image_of(e,texture)&&my_glGetError(e)==0x501&&!my_glGetError(e));
    my_glTexImage2D(e,GL_TEXTURE_2D,1,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    assert(image_of(e,texture)&&!my_glGetError(e));
    glGenFramebuffers(1,&fb);my_glBindFramebuffer(e,GL_FRAMEBUFFER,fb);
    my_glFramebufferTexture2D(e,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(1,0,0,1);my_glClear(e,GL_COLOR_BUFFER_BIT);
    host_lock_error=1;
    my_glTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    assert(image_of(e,texture)&&image_of(e,texture)->drawn&&my_glGetError(e)==0x502);host_lock_error=0;
    glEnable(0xffffffffu); // An unrelated earlier error must survive a successful definition.
    my_glTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    assert(!image_of(e,texture)&&my_glGetError(e)==0x500&&!my_glGetError(e));
    for(unsigned n=0;n<16;n++)assert(bytes[4*n]==0&&bytes[4*n+1]==0&&bytes[4*n+2]==255&&bytes[4*n+3]==255);
    assert(surface.seed==2);check_errors=errors;
    glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fb);if(e->reader)glDeleteFramebuffers(1,&e->reader);free(e);
    puts("PASS rejected storage/mip definition retain ownership; failed publication retries; old pixels publish before replacement; prior errors remain visible");
}
static void test_shared_storage_replacement(void){
    struct eagl *e=calloc(1,sizeof(*e));assert(e);unsigned char bytes[64]={0},fresh[64];memset(fresh,255,sizeof fresh);
    struct host_surface surface={201,0,'BGRA',0,4,4,4,16,64,bytes};
    GLuint owner,texture;glGenTextures(1,&owner);glBindTexture(GL_TEXTURE_2D,owner);
    unsigned char red[64];for(unsigned n=0;n<16;n++){red[4*n]=255;red[4*n+1]=red[4*n+2]=0;red[4*n+3]=255;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,red);
    extern void *eglGetCurrentContext(void);const int attrs[]={0x30d2,1,0x3038};
    imported_image=create_image(egl_display,eglGetCurrentContext(),0x30b1,(void *)(uintptr_t)owner,attrs);assert(imported_image);
    glGenTextures(1,&texture);my_glBindTexture(e,GL_TEXTURE_2D,texture);
    assert(my_texImageIOSurface(e,NULL,&surface,GL_TEXTURE_2D,GL_RGBA,4,4,GL_BGRA,GL_UNSIGNED_BYTE,0,NO));
    assert(image_of(e,texture)->shared);my_glTexImage2D(e,GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    assert(!image_of(e,texture)&&!my_glGetError(e));unsigned char pixel[4];read_pixel(owner,pixel);
    assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
    assert(destroy_image(egl_display,imported_image));imported_image=NULL;
    glDeleteTextures(1,&owner);glDeleteTextures(1,&texture);free(e);
    puts("PASS same-size storage replacement leaves imported EGLImage owner pixels unchanged");
}
#endif

int main(void){context();
#ifdef EAGL_STORAGE_OWNERSHIP
    test_storage_failures_and_publish();test_shared_storage_replacement();
#endif
#if defined(EAGL_STORAGE_OWNERSHIP) || defined(EXPECT_STALE_STORAGE)
    for(unsigned m=0;m<3;m++)for(unsigned b=0;b<2;b++)test_storage_replacement(m,b);
#endif
#ifdef EXPECT_STALE_STORAGE
    return 0;
#endif
#ifdef EXPECT_RECYCLED_SURFACE
    test_pool_lifetime();return 0;
#endif
#if defined(EAGL_SUBIMAGE_PUBLISH) || defined(EXPECT_UNPUBLISHED_SUBIMAGE)
    test_subimage_publication(false,false);
#ifdef EXPECT_UNPUBLISHED_SUBIMAGE
    return 0;
#endif
    test_subimage_publication(true,false);test_subimage_publication(false,true);test_subimage_publication(true,true);
#endif
#ifdef EAGL_POOL_USES
    test_pool_lifetime();
#endif
#ifdef EXPECT_STALE_UPLOAD
    test_upload_seed_race(false);test_upload_seed_race(true);
    puts("REPRODUCED stale white snapshot and left black column retained across 20 frames after concurrent publication");return 0;
#endif
    test_registration_group(false);test_registration_group(true);
#ifdef EXPECT_STALE_GROUP
    puts("REPRODUCED stale group materialization after both IOSurface registration routes");return 0;
#endif
    puts("PASS IOSurface registration clears former material state; rejection preserves it; ordinary groups still materialize");
    test_upload_seed_race(false);test_upload_seed_race(true);
    puts("PASS concurrent publication: white snapshot and left black column refresh, stable binds avoid reupload, row padding retained");
    for(unsigned r=0;r<2;r++)for(unsigned i=0;i<2;i++)test_mirror_handoff(r,i);
    puts("PASS real EGLImage retirement: deletion/reimport, orientation, complete left edge, failed publication retry and caller GL state");
    for(unsigned k=0;k<4;k++)for(unsigned inv=0;inv<2;inv++)test_surface_handoff(k,inv,k!=0);
    puts("PASS publication before delete, same/different surface registration, and alias import; deferred full/left-edge clear, lock failure preservation, orientation and padding");test_external_target_refresh();test_mip_refresh();
#ifndef EXPECT_STALE_MIPMAP
    test_shared_mip();
#endif

#ifdef EXPECT_CONTAMINATED_UPLOAD
    test_cpu_upload(false,false,false,false,4);test_cpu_upload(true,false,false,false,4);return 0;
#endif

#ifdef EXPECT_SHARED_RENDER
    test_shared_render(true,false);test_shared_render(true,true);return 0;
#endif

#ifdef EXPECT_STALE_READ
    test_read_boundaries(false);test_read_boundaries(true);puts("REPRODUCED: ReadPixels and CopyTexImage2D capture old pixels before deferred clear");return 0;
#endif
#ifdef EXPECT_UNFLIPPED
    test_mirror(true,false);test_mirror(true,true);puts("REPRODUCED: inverted GPU mirror keeps source rows in ascending order");return 0;
#endif
#ifndef EXPECT_UNPUBLISHED
    test_unseeded_refresh();test_snapshot_attachment(false);test_snapshot_attachment(true);
    test_cpu_upload(false,false,false,false,4);test_cpu_upload(true,false,false,false,4);
    test_cpu_upload(true,true,false,false,4);test_cpu_upload(true,true,true,false,4);test_cpu_upload(true,true,false,true,4);
    test_cpu_upload(false,false,false,false,0);
#endif
    test_independent_reads();test_read_boundaries(false);test_read_boundaries(true);test_surface(false);test_surface(true);
#ifndef EXPECT_UNPUBLISHED
    struct eagl *late=calloc(1,sizeof(*late));assert(late);late->frames=700;check_errors=0;
    glEnable(0xffffffffu);frame_end(late,false);assert(late->errors==1&&!glGetError());free(late);check_errors=1;
    test_shared_one_pixel();test_shared_render(true,false);test_shared_render(false,false);test_shared_render(true,true);test_shared_render(false,true);test_mirror(false,false);test_mirror(true,false);test_mirror(true,true);test_copy();test_rejections();puts("PASS CPU upload/refresh with inherited skips and unpack buffer, actual EGLImage sibling detachment (including 1x1), GPU pixel preservation, partial render/copy, mirror/CPU publication, failure state, surface orientation/padding, seed ownership and GL binding/pack/unpack/framebuffer state");
#endif
}
