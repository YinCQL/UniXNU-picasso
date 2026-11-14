// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "PicassoAudioMix.h"
static PicassoAudioMix mix;
int main(void){
 float in[8]={0.25f,-0.25f,NAN,INFINITY,1,-1,0.5f,0};int16_t out[8],again[8];
 assert(!picasso_mix_add(&mix,100,100,4,in,1));assert(!picasso_mix_add(&mix,100,100,4,in,1));
 assert(!picasso_mix_peek(&mix,100,4,out));assert(out[0]==16383&&out[1]==-16383&&out[2]==0&&out[3]==0&&out[4]==32767&&out[5]==-32767);
 assert(!picasso_mix_peek(&mix,100,4,again)&&!memcmp(out,again,sizeof out));
 assert(picasso_mix_add(&mix,100,99,4,in,1)==-1);assert(picasso_mix_add(&mix,100,100+PicassoMixFrames-3,4,in,1)==-1);
 picasso_mix_consume(&mix,100,4);assert(picasso_mix_peek(&mix,100,4,out)==4&&out[0]==0);
 assert(!picasso_mix_add(&mix,65535,65535,4,in,0.5f));assert(!picasso_mix_peek(&mix,65535,4,out)&&out[0]==4095);
 assert(picasso_mix_add(&mix,0,0,8193,in,1)==-1&&picasso_mix_add(&mix,0,0,1,0,1)==-1);
 puts("PASS actual HAL mix: overlap, finite conversion, clipping, stale/future bounds, backpressure and wrap");return 0;
}
