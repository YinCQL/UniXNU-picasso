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


static const char state_fragment[] =
    "#version 300 es\nprecision highp float;uniform vec4 flat_colour;out vec4 result;void main(){result=flat_colour;}";
static const GLfloat vertices[] = {-1,-1,1,-1,-1,1,1,1};
static void verify_state(GLuint read, GLuint draw, GLuint program, GLuint texture, GLboolean mask[4]) {
    GLint actual,view[4],box[4];GLboolean bits[4];
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&actual);assert(actual==(GLint)read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&actual);assert(actual==(GLint)draw);
    glGetIntegerv(0x8b8d,&actual);assert(actual==(GLint)program);
    glGetIntegerv(GL_VIEWPORT,view);assert(view[0]==1&&view[1]==2&&view[2]==3&&view[3]==4);
    glGetIntegerv(GL_SCISSOR_BOX,box);assert(box[0]==2&&box[1]==1&&box[2]==1&&box[3]==1);
    assert(glIsEnabled(GL_SCISSOR_TEST)&&glIsEnabled(GL_BLEND));
    glGetBooleanv(GL_COLOR_WRITEMASK,bits);assert(!memcmp(bits,mask,4));
    glGetIntegerv(0x84e0,&actual);assert(actual==GL_TEXTURE0+2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&actual);assert(actual==(GLint)texture);
    for (unsigned i=0;i<3;i++) {glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&actual);assert(!actual);}
    assert(!glGetError());
}
static void caller_state(GLuint read,GLuint draw,GLuint program,GLuint texture,GLboolean mask[4]) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
    glUseProgram(program);glViewport(1,2,3,4);glEnable(GL_SCISSOR_TEST);glScissor(2,1,1,1);glEnable(GL_BLEND);
    glColorMask(mask[0],mask[1],mask[2],mask[3]);glActiveTexture(GL_TEXTURE0+2);glBindTexture(GL_TEXTURE_2D,texture);
}
int main(void) {
    context();struct eagl e={0};struct group_texture group={.state=GROUP_VIBRANT};
    GLuint tex,fb,read,second;unsigned char rgba[4];
    glGenTextures(1,&tex);glGenTextures(1,&second);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb);glGenFramebuffers(1,&read);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    e.framebuffer=fb;e.bound[0]=tex;e.unit=2;
    for(unsigned i=0;i<3;i++){e.arrays[i].size=2;e.arrays[i].type=GL_FLOAT;e.arrays[i].pointer=vertices;glDisableVertexAttribArray(i);}
    /* Compile the production shaders. The nonzero draw uses a surrogate because llvmpipe's
       framebuffer-fetch JIT crashes in LLVMTypeOf; that shader's pixel math is not tested here. */
    assert(group_programs(&e));
    e.group_cover=plain_program(state_fragment,"state fixture");e.group_over=e.group_cover;
    e.group_flat=plain_program(state_fragment,"state fixture");
    e.group_flat_matrix=glGetUniformLocation(e.group_flat,"vertex_matrix");
    e.group_flat_colour=glGetUniformLocation(e.group_flat,"flat_colour");
    GLboolean mask[4]={0,0,0,1};
    glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
    caller_state(read,fb,e.group_over,second,mask);
    group_flatten(&e,tex,&group);
    verify_state(read,fb,e.group_over,second,mask);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    assert(!rgba[0]&&!rgba[1]&&!rgba[2]&&!rgba[3]);
    /* Reproduce a nested draw: actual program differs from the tracked CoreAnimation program. */
    e.program=0;group.state=GROUP_VIBRANT;group.colours[19]=1;group.colours[16]=1;
    caller_state(read,fb,e.group_over,second,mask);group_flatten(&e,tex,&group);
    verify_state(read,fb,e.group_over,second,mask);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    assert(rgba[0]==255&&!rgba[1]&&!rgba[2]&&rgba[3]==255);
    /* A rejected internal program still restores bindings and colour writes. */
    e.group_cover=0;e.group_failed=true;group.state=GROUP_VIBRANT;
    caller_state(read,fb,e.group_over,second,mask);group_flatten(&e,tex,&group);
    verify_state(read,fb,e.group_over,second,mask);
    /* Error observations query the active target without consuming an existing GL error. */
    e.frames=336;e.draw_function=45;captured_lines=0;
    compositor_framebuffer(&e,"fixture");
    assert(captured_lines==2&&strstr(captured,"attachment=8ce0 type=1702"));
    verify_state(read,fb,e.group_over,second,mask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,read);captured_lines=0;
    glEnable(0xffffffff); /* GL_INVALID_ENUM remains owned by the original error consumer. */
    compositor_framebuffer(&e,"fixture-missing");
    assert(captured_lines==1&&strstr(captured,"status=8cd7"));
    assert(glGetError()==0x500);assert(glGetError()==0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,(GLint*)&fb);assert(fb==read);
    puts("PASS group state: transparent RGBA clear, read/draw FBO, nested program, masks, viewport, scissor, blend, textures, attributes and bounded error observations");
    return 0;
}
