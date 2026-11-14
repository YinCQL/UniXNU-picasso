// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "picasso-snapshot-resample-core.h"

static unsigned random_state=123456789;
static unsigned next_byte(void) { random_state=1664525*random_state+1013904223; return random_state>>24; }
static unsigned oracle(const unsigned char *src,size_t sw,size_t sh,size_t sr,size_t tw,size_t th,size_t x,size_t y,unsigned c)
{
    double px=((double)x+0.5)*sw/tw-0.5,py=((double)y+0.5)*sh/th-0.5;
    if(px<0)px=0;if(py<0)py=0;if(px>sw-1)px=sw-1;if(py>sh-1)py=sh-1;
    size_t x0=(size_t)floor(px),y0=(size_t)floor(py),x1=x0+1<sw?x0+1:x0,y1=y0+1<sh?y0+1:y0;
    double fx=px-x0,fy=py-y0;
    double a=src[y0*sr+x0*4+c]*(1-fx)+src[y0*sr+x1*4+c]*fx;
    double b=src[y1*sr+x0*4+c]*(1-fx)+src[y1*sr+x1*4+c]*fx;
    return (unsigned)floor(a*(1-fy)+b*fy+0.5);
}
static void check(size_t sw,size_t sh,size_t tw,size_t th,unsigned style)
{
    size_t sr=sw*4+32,tr=tw*4+48,sn=sr*sh,tn=tr*th;
    unsigned char *s=malloc(sn+128),*copy=malloc(sn+128),*t=malloc(tn+128);assert(s&&copy&&t);
    memset(s,0x91,sn+128);memset(t,0xa5,tn+128);
    for(size_t y=0;y<sh;y++)for(size_t x=0;x<sw;x++){
        unsigned char *p=s+64+y*sr+x*4;
        if(style==0)memset(p,255,4);
        else if(style==1){p[0]=(x>sw/3&&x<sw/2&&y>sh/4&&y<sh/3)?64:242;p[1]=p[0];p[2]=p[0];p[3]=255;}
        else if(style==2){p[3]=next_byte();for(unsigned c=0;c<3;c++)p[c]=next_byte()%(p[3]+1);}
        else for(unsigned c=0;c<4;c++)p[c]=next_byte();
    }
    memcpy(copy,s,sn+128);assert(picasso_snapshot_resample(s+64,sw,sh,sr,t+64,tw,th,tr));
    assert(!memcmp(copy,s,sn+128));
    for(size_t i=0;i<64;i++)assert(t[i]==0xa5&&t[tn+64+i]==0xa5);
    for(size_t y=0;y<th;y++){
        for(size_t x=tw*4;x<tr;x++)assert(t[64+y*tr+x]==0xa5);
        for(size_t x=0;x<tw;x++)for(unsigned c=0;c<4;c++){
            int actual=t[64+y*tr+x*4+c],expected=oracle(s+64,sw,sh,sr,tw,th,x,y,c);
            assert(abs(actual-expected)<=1);
            if(style==0)assert(actual==255);
            if(sw==tw&&sh==th)assert(actual==s[64+y*sr+x*4+c]);
            if(style==2&&c<3)assert(actual<=t[64+y*tr+x*4+3]);
        }
    }
    free(s);free(copy);free(t);
}
static void check_bgrx(void)
{
    unsigned char source[16*9*4],original[sizeof(source)],target[12*7*4],expected[sizeof(target)];
    for(size_t n=0;n<sizeof(source);n++)source[n]=next_byte();memcpy(original,source,sizeof(source));
    assert(picasso_snapshot_resample(source,16,9,16*4,expected,12,7,12*4));
    assert(picasso_snapshot_resample_opaque(source,16,9,16*4,target,12,7,12*4));
    assert(!memcmp(source,original,sizeof(source)));
    for(size_t n=0;n<sizeof(target);n++)assert(target[n]==(n%4==3?255:expected[n]));
    memset(target,0xa5,sizeof(target));assert(!picasso_snapshot_resample_opaque(source,0,9,16*4,target,12,7,12*4));
    for(size_t n=0;n<sizeof(target);n++)assert(target[n]==0xa5);
}
int main(void)
{
    check_bgrx();unsigned char a[64]={0},b[64];memset(b,0xa5,sizeof(b));
    assert(!picasso_snapshot_resample(NULL,1,1,4,b,1,1,4));
    assert(!picasso_snapshot_resample(a,0,1,4,b,1,1,4));
    assert(!picasso_snapshot_resample(a,4097,1,16388,b,1,1,4));
    assert(!picasso_snapshot_resample(a,1,1,4,b,2,1,8));
    assert(!picasso_snapshot_resample(a,2,2,7,b,1,1,4));
    assert(!picasso_snapshot_resample(a,2,2,SIZE_MAX,b,1,1,4));
    assert(!picasso_snapshot_resample(a,2,2,8,b,1,1,3));
    assert(!picasso_snapshot_resample(a,2,2,8,a+4,1,1,4));
    for(size_t i=0;i<sizeof(b);i++)assert(b[i]==0xa5);
    check(1080,2400,756,1680,0);check(1080,2400,756,1680,1);
    for(unsigned style=0;style<4;style++){
        check(1,1,1,1,style);check(13,17,13,17,style);check(31,27,17,13,style);check(63,59,1,1,style);check(4096,1,4095,1,style);
    }
    for(unsigned n=0;n<50;n++){size_t w=1+next_byte(),h=1+next_byte();check(w,h,1+next_byte()%w,1+next_byte()%h,2);}
    puts("PASS: BGRX ignored-byte/opaque-alpha plus 72 images, float oracle (<=1 channel unit), opaque edges, premultiplied alpha, stride padding, guards, identity and rejection checks");
}
