// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise production readonly surface sampling with explicit API fixtures.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct fixture { size_t w,h,row,bytes,element,planes; uint32_t format,seed,next; int lock,unlock; unsigned reads,locks,unlocks; unsigned char pixels[80]; };
typedef struct fixture *IOSurfaceRef;
enum { kIOSurfaceLockReadOnly=1 };
static uint32_t IOSurfaceGetID(IOSurfaceRef s){return s?7:0;}
static int IOSurfaceLock(IOSurfaceRef s,unsigned flags,uint32_t *seed){assert(flags==1);++s->locks;*seed=s->seed;return s->lock;}
static int IOSurfaceUnlock(IOSurfaceRef s,unsigned flags,uint32_t *seed){assert(flags==1);++s->unlocks;*seed=s->next;return s->unlock;}
#define GET(name,member) static size_t name(IOSurfaceRef s){return s->member;}
GET(IOSurfaceGetWidth,w) GET(IOSurfaceGetHeight,h) GET(IOSurfaceGetBytesPerRow,row)
GET(IOSurfaceGetAllocSize,bytes) GET(IOSurfaceGetBytesPerElement,element) GET(IOSurfaceGetPlaneCount,planes)
static uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s){return s->format;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s){++s->reads;return s->pixels;}
#include "picasso-wallpaper-surface.h"
static struct fixture good(void){
 struct fixture s={.w=4,.h=4,.row=20,.bytes=80,.element=4,.format=0x42475241,.seed=5,.next=5};
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){unsigned char *p=s.pixels+y*20+x*4;p[0]=x+1;p[1]=y+1;p[2]=7;p[3]=255;}
 return s;
}
static void w30r_words(struct fixture *s,unsigned a,unsigned b,unsigned c,unsigned pad){
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){unsigned char *p=s->pixels+y*20+x*4;
  uint32_t v=a|(b<<10)|(c<<20)|(pad<<30);for(unsigned k=0;k<4;++k)p[k]=(unsigned char)(v>>(8*k));}
}
static void test_w30r(void){
 struct picasso_wallpaper_surface_result r;struct fixture s=good();s.format=0x77333072;
 w30r_words(&s,384,384,384,0);unsigned char before[80];memcpy(before,s.pixels,80);
 assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.pixels.count==16);
 assert(!r.pixels.rgb_nonzero&&!r.pixels.alpha_nonzero&&r.w30r_black==16&&!r.w30r_zero&&!r.w30r_padding&&r.w30r_min==384&&r.w30r_max==384);
 assert(!memcmp(before,s.pixels,80));
 w30r_words(&s,0,0,0,0);assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels"));
 assert(r.w30r_zero==16&&!r.w30r_black&&!r.pixels.rgb_nonzero&&r.w30r_min==0&&r.w30r_max==0);
 w30r_words(&s,383,384,384,0);assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&!r.pixels.rgb_nonzero&&r.w30r_min==383);
 w30r_words(&s,385,895,1023,0);assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.pixels.rgb_nonzero==16&&!r.pixels.alpha_nonzero&&r.w30r_min==385&&r.w30r_max==1023);
 w30r_words(&s,384,384,384,3);assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.w30r_padding==16&&!r.pixels.rgb_nonzero&&!r.pixels.alpha_nonzero);
 w30r_words(&s,384,384,384,0);s.pixels[0]=0x81;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.pixels.varied==15&&r.pixels.rgb_nonzero==1);
 s.bytes=75;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"no-linear-pixels")&&!r.pixels.count);s.bytes=80;
 s.next=6;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"seed-changed")&&!r.pixels.count&&!r.w30r_min&&!r.w30r_max&&!r.w30r_black);s.next=5;
 s.unlock=-1;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"unlock-failed")&&!r.pixels.count&&!r.w30r_padding);s.unlock=0;
 s.format=0x6c313072;unsigned reads=s.reads;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"metadata")&&s.reads==reads);
}
int main(void){
 test_w30r();
 struct picasso_wallpaper_surface_result r;assert(!strcmp(picasso_wallpaper_sample_surface(0,&r),"no-surface")&&!r.pixels.count);
 struct fixture s=good(),before=s;assert(picasso_sample_rgba(s.pixels,s.bytes,4,4,s.row,&r.pixels));assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels"));
 assert(r.pixels.count==16&&r.pixels.rgb_nonzero==16&&r.pixels.alpha_nonzero==16&&r.identifier==7&&s.locks==1&&s.unlocks==1&&s.reads==1);
 assert(!memcmp(before.pixels,s.pixels,80));
 s=good();memset(s.pixels,0,80);assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.pixels.rgb_nonzero==0);
 for(unsigned i=0;i<9;++i){s=good();switch(i){case 0:s.planes=1;break;case 1:s.format=0;break;case 2:s.element=8;break;case 3:s.w=8193;break;case 4:s.h=8193;break;case 5:s.w=0;break;case 6:s.bytes=64*1024*1024+1;break;case 7:s.w=8192;s.h=8192;break;default:s.h=0;}
  assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"metadata")&&!r.pixels.count&&!s.reads&&s.locks==1&&s.unlocks==1);}
 s=good();s.lock=-1;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"lock-failed")&&!s.reads&&!s.unlocks);
 s=good();s.unlock=-2;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"unlock-failed")&&!r.pixels.count&&s.unlocks==1);
 s=good();s.next=6;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"seed-changed")&&!r.pixels.count);
 s=good();s.row=15;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"no-linear-pixels")&&!r.pixels.count);
 s=good();s.bytes=75;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"no-linear-pixels")&&!r.pixels.count);
 s=good();s.row=SIZE_MAX;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"no-linear-pixels")&&!r.pixels.count);
 s=good();s.format=0x52474241;assert(!strcmp(picasso_wallpaper_sample_surface(&s,&r),"pixels")&&r.pixels.count==16);
 puts("PASS readonly surface: native w30r black384/zero/positive range/padding/no-alpha/stable seed plus colored/black packed/padded pixels, unsupported formats/planes/HDR/size, lock/unlock/seed, row/length bounds and unchanged bytes");return 0;
}
