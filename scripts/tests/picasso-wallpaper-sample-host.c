// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "picasso-pixel-sample.h"
static char last[400];
static void picasso_pixel_console(const char *line, int length, size_t capacity)
{ assert(length > 0 && (size_t)length < capacity); memcpy(last,line,(size_t)length+1); }
static int getpid(void) { return 26; }
#include "picasso-astc-guard-core.h"
static uintptr_t picasso_astc_texture_vtable = 0x12345678;
#include "astc-note.inc"
#include "wallpaper-budget.inc"
int main(void)
{
    for(unsigned kind=0;kind<5;++kind){
        assert(picasso_wallpaper_take(kind,0) && picasso_wallpaper_take(kind,0));
        for(unsigned n=0;n<10000;++n)assert(!picasso_wallpaper_take(kind,0));
        for(unsigned n=0;n<16;++n)assert(picasso_wallpaper_take(kind,1));
        for(unsigned n=0;n<10000;++n)assert(!picasso_wallpaper_take(kind,1));
    }
    assert(!picasso_wallpaper_take(5,1) && !picasso_wallpaper_take(UINT32_MAX,0));
    unsigned char pixels[4 * 4 * 4 + 16], before[sizeof(pixels)];
    struct picasso_pixel_sample sample;
    struct picasso_astc_request request={.linear_bytes=16};
    assert(picasso_astc_layout_fits(&request,16));
    memset(pixels,0,sizeof(pixels));
    assert(picasso_sample_rgba(pixels,sizeof(pixels),4,4,16,&sample));
    assert(sample.count==16 && !sample.rgb_nonzero && !sample.alpha_nonzero && !sample.varied);
    for(unsigned i=0;i<16;++i)pixels[i*4+3]=255;
    assert(picasso_sample_rgba(pixels,64,4,4,16,&sample));
    assert(!sample.rgb_nonzero && sample.alpha_nonzero==16 && !sample.varied);
    pixels[4]=42;
    memcpy(before,pixels,sizeof(pixels));
    assert(picasso_sample_rgba(pixels,64,4,4,16,&sample));
    assert(sample.rgb_nonzero==1 && sample.varied==1 && !memcmp(before,pixels,sizeof(pixels)));
    unsigned char argb[4]={255,0,0,0};
    assert(picasso_sample_rgb32(argb,4,1,1,4,&sample,1));
    assert(sample.count==16 && !sample.rgb_nonzero && sample.alpha_nonzero==16);
    argb[1]=1;assert(picasso_sample_rgb32(argb,4,1,1,4,&sample,1) && sample.rgb_nonzero==16);
    assert(!picasso_sample_rgba(NULL,64,4,4,16,&sample));
    assert(!picasso_sample_rgba(pixels,64,0,4,16,&sample));
    assert(!picasso_sample_rgba(pixels,64,4,0,16,&sample));
    assert(!picasso_sample_rgba(pixels,63,4,4,16,&sample));
    assert(!picasso_sample_rgba(pixels,64,4,4,15,&sample));
    assert(!picasso_sample_rgba(pixels,SIZE_MAX,UINT32_MAX,UINT32_MAX,SIZE_MAX,&sample));
    unsigned char *padding=malloc(88);assert(padding);memset(padding,0x77,88);
    for(unsigned y=0;y<4;++y) memset(padding+y*24,0,16);
    assert(picasso_sample_rgba(padding,88,4,4,24,&sample) && !sample.rgb_nonzero);
    free(padding);
    unsigned char plugin[0x250]={0};uint32_t w=4,h=4,row=16;
    unsigned char texture[0xe0]={0};
    uintptr_t ptr=(uintptr_t)texture;memcpy(plugin+0x1c0,&ptr,sizeof(ptr));
    memcpy(texture,&picasso_astc_texture_vtable,sizeof(ptr));
    memcpy(plugin+0xfc,&w,4);memcpy(plugin+0x100,&h,4);memcpy(plugin+0x124,&row,4);
    /* Failed decoders may leave an invalid/uninitialized pointer. Never read it. */
    picasso_astc_decode_note(plugin,(void *)1,64,0,-50);
    assert(strstr(last,"result=-50") && strstr(last,"samples=0"));
    picasso_astc_decode_note(plugin,pixels,64,0,0);
    assert(strstr(last,"size=4/4 row=16") && strstr(last,"samples=16 rgb_nonzero=1"));
    texture[0x2a]=1;picasso_astc_decode_note(plugin,(void *)1,64,0,0);
    assert(strstr(last,"samples=0"));texture[0x2a]=0;
    for(unsigned i=0;i<13;++i)picasso_astc_decode_note(plugin,pixels,64,0,0);
    strcpy(last,"unchanged");picasso_astc_decode_note(plugin,(void *)1,64,0,0);
    assert(!strcmp(last,"unchanged"));
    picasso_astc_decode_note(plugin,(void *)1,64,1,-4);
    assert(strstr(last,"kind=atx result=-4") && strstr(last,"samples=0"));
    puts("PASS pixel grids, alpha positions, padding, exact bounds, failed decode unread, independent bounded budgets");
    return 0;
}
