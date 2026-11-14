#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int GLint,GLsizei;
typedef unsigned GLuint,GLboolean;
typedef void *GLsync;
enum {GL_READ_FRAMEBUFFER_BINDING=1,GL_DRAW_FRAMEBUFFER_BINDING,GL_READ_FRAMEBUFFER,GL_DRAW_FRAMEBUFFER,
 GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,GL_SCISSOR_TEST,GL_COLOR_BUFFER_BIT,GL_NEAREST,GL_FRAMEBUFFER,
 GL_PACK_ROW_LENGTH,GL_PACK_ALIGNMENT,GL_BGRA,GL_UNSIGNED_BYTE,GL_SYNC_GPU_COMMANDS_COMPLETE,GL_SYNC_FLUSH_COMMANDS_BIT};
struct surface { unsigned seeds; size_t row; uint32_t *pixels; };
typedef struct surface *IOSurfaceRef;
struct image {IOSurfaceRef surface;GLint width,height,dmg[4];GLuint texture,mirror;bool drawn,damaged,invert,shared,target;};
struct eagl {GLuint reader,writer,framebuffer;unsigned image_count;struct image images[3];};
static unsigned damage_blits=1,wait_for_frames=1,sync_frames=1,shown_texture;
static GLint read_fb=71,draw_fb=72,box[4]={0,0,20,2};
static bool scissor=true,queued;
static unsigned finishes,flushes,blits;
static GLint rectangle[8];
static uint32_t source[1080*4];
static struct image *current;
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s){return s->row;}
static int IOSurfaceLock(IOSurfaceRef s,unsigned flags,void *seed){(void)s;(void)flags;(void)seed;assert(!queued);return 0;}
static void IOSurfaceUnlock(IOSurfaceRef s,unsigned flags,void *seed){(void)flags;(void)seed;s->seeds++;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s){return s->pixels;}
static void glGetIntegerv(unsigned what,GLint *v){*v=what==GL_READ_FRAMEBUFFER_BINDING?read_fb:draw_fb;}
static void glGenFramebuffers(unsigned n,GLuint *v){assert(n==1);*v=100;}
static void glBindFramebuffer(unsigned what,GLuint v){if(what==GL_READ_FRAMEBUFFER)read_fb=(GLint)v;else draw_fb=(GLint)v;}
static void glFramebufferTexture2D(unsigned a,unsigned b,unsigned c,GLuint d,unsigned f){(void)a;(void)b;(void)c;(void)d;(void)f;}
static GLboolean glIsEnabled(unsigned what){assert(what==GL_SCISSOR_TEST);return scissor;}
static void glDisable(unsigned what){assert(what==GL_SCISSOR_TEST);scissor=false;}
static void glEnable(unsigned what){assert(what==GL_SCISSOR_TEST);scissor=true;}
static void glBlitFramebuffer(GLint a,GLint b,GLint c,GLint d,GLint e,GLint f,GLint g,GLint h,unsigned mask,unsigned filter){
 assert(!scissor && mask==GL_COLOR_BUFFER_BIT && filter==GL_NEAREST && !queued);
 GLint v[]={a,b,c,d,e,f,g,h};memcpy(rectangle,v,sizeof v);queued=true;blits++;
}
static void glFinish(void){
 finishes++;
 if(queued){
  int y0=rectangle[1],y1=rectangle[3],x0=rectangle[0],x1=rectangle[2];
  for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
   int dy=current->invert?current->height-1-y:y;
   current->surface->pixels[dy*1080+x]=source[y*1080+x];
  }
  queued=false;
 }
}
static void glFlush(void){flushes++;}
static GLsync glFenceSync(unsigned a,unsigned b){(void)a;(void)b;return (void *)1;}
static int glClientWaitSync(GLsync a,unsigned b,unsigned c){(void)a;(void)b;(void)c;return 0;}
static void glDeleteSync(GLsync s){(void)s;}
static void glPixelStorei(unsigned a,int b){(void)a;(void)b;}
static void glReadPixels(int a,int b,int c,int d,unsigned e,unsigned f,void *g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;assert(false);}
static void show_texture(struct eagl *e,struct image *im){(void)e;(void)im;assert(false);}
static uint64_t now_us(void){return 1;}
#include "gl-handoff.inc"
int main(void){
 uint32_t pixels[1080*4];struct surface surface={0,4320,pixels};
 for(unsigned invert=0;invert<2;invert++)for(unsigned enabled=0;enabled<2;enabled++)for(unsigned finished=0;finished<2;finished++){
  for(unsigned i=0;i<1080*4;i++){source[i]=i+1;pixels[i]=0;}
  struct eagl e={0};e.image_count=1;e.images[0]=(struct image){.surface=&surface,.width=1080,.height=4,.texture=1,.mirror=2,.drawn=true,.invert=invert,.shared=true,.target=true};
  current=&e.images[0];scissor=enabled;read_fb=71;draw_fb=72;surface.seeds=0;finishes=flushes=blits=0;wait_for_frames=!finished;
  frame_handoff(&e,finished);
  assert(finishes==1 && flushes==1 && blits==1 && !queued && surface.seeds==1 && !current->drawn);
  assert(scissor==enabled && read_fb==71 && draw_fb==72);
  assert(box[2]==20);
  for(unsigned y=0;y<4;y++)for(unsigned x=0;x<1080;x++)assert(pixels[y*1080+x]==source[(invert?3-y:y)*1080+x]);
 }
 struct eagl e={0};e.image_count=1;e.images[0]=(struct image){.surface=&surface,.width=1080,.height=4,.texture=1,.mirror=2,.drawn=true,.damaged=true,.dmg={1077,1,50,2},.target=true};current=&e.images[0];
 memset(pixels,0,sizeof pixels);finishes=flushes=blits=0;surface.seeds=0;wait_for_frames=1;frame_handoff(&e,false);
 assert(rectangle[0]==1077 && rectangle[2]==1080 && surface.seeds==1);
 assert(pixels[1*1080+1079]==source[1*1080+1079] && pixels[1*1080+1076]==0);
 current->drawn=current->damaged=true;current->dmg[2]=0;surface.seeds=0;finishes=0;frame_handoff(&e,true);
 assert(!surface.seeds && !finishes && !current->drawn);
 current->mirror=0;current->shared=current->drawn=true;surface.seeds=0;finishes=0;frame_handoff(&e,true);
 assert(surface.seeds==1 && finishes==0);
 puts("PASS scissor restoration, 1080 right edge, inversion, damage clamp, completed publication and finish reuse");
}
