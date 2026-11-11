// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl-body.inc"
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
static void pixel(unsigned char *p){glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(!glGetError());}
int main(void){
    context();struct eagl e={0};GLuint tex,fb,other;unsigned char pixels[16*16*4],p[4];
    for(unsigned i=0;i<256;i++){pixels[4*i]=40;pixels[4*i+1]=80;pixels[4*i+2]=160;pixels[4*i+3]=255;}
    glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(GL_TEXTURE_2D,0x2801,0x2601);glTexParameteri(GL_TEXTURE_2D,0x2800,0x2601);
    glGenFramebuffers(1,&fb);glGenFramebuffers(1,&other);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    e.colour=e.bound[0]=tex;e.framebuffer=fb;glViewport(0,0,16,16);glClearColor(0,0,0,0);
    my_glClear(&e,GL_COLOR_BUFFER_BIT);resolve_feedback_clear(&e,61);clear_now(&e);pixel(p);
    assert(!memcmp(p,pixels,4));
    // The mask at Clear time matters even if the application changes it before Draw.
    e.bound[0]=0;glColorMask(1,0,0,0);glClearColor(1,1,1,1);my_glClear(&e,GL_COLOR_BUFFER_BIT);
    glColorMask(0,1,0,0);resolve_feedback_clear(&e,1);clear_now(&e);pixel(p);
    assert(p[0]==255&&p[1]==80&&p[2]==160&&p[3]==255);
    GLboolean mask[4];glGetBooleanv(GL_COLOR_WRITEMASK,mask);assert(!mask[0]&&mask[1]&&!mask[2]&&!mask[3]);
    // Ordinary draws and unknown feedback functions still perform their clears.
    glColorMask(1,1,1,1);glClearColor(0,0,0,0);my_glClear(&e,GL_COLOR_BUFFER_BIT);resolve_feedback_clear(&e,1);clear_now(&e);pixel(p);
    assert(!p[0]&&!p[1]&&!p[2]&&!p[3]);
    e.bound[0]=tex;my_glClear(&e,GL_COLOR_BUFFER_BIT|0x100);resolve_feedback_clear(&e,38);assert(e.unclear==0x100);clear_now(&e);
    e.program=1;my_glClear(&e,GL_COLOR_BUFFER_BIT);resolve_feedback_clear(&e,61);assert(e.unclear==GL_COLOR_BUFFER_BIT);clear_now(&e);e.program=0;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,other);glColorMask(0,0,0,1);glEnable(GL_SCISSOR_TEST);glScissor(7,7,1,1);glEnable(GL_BLEND);
    vblur_level(&e,0);assert(e.chains[0].made==2);
    GLint read,draw;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
    assert(read==(GLint)other&&draw==(GLint)fb);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    assert(!mask[0]&&!mask[1]&&!mask[2]&&mask[3]&&glIsEnabled(GL_SCISSOR_TEST)&&glIsEnabled(GL_BLEND));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,e.chains[0].framebuffer[1]);pixel(p);assert(!memcmp(p,pixels,4));
    puts("PASS real software GL pixels: feedback preservation, deferred masks, blur RGBA, independent framebuffer restoration");
}
