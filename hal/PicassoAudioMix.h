// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_MIX_H
#define PICASSO_AUDIO_MIX_H
#include <stdint.h>
#include <math.h>
enum {PicassoMixFrames=65536};
typedef struct {float sample[PicassoMixFrames][2];uint64_t tag[PicassoMixFrames];} PicassoAudioMix;
static inline int picasso_mix_add(PicassoAudioMix *m,uint64_t consumed,uint64_t at,unsigned frames,const float *in,float gain){
    if(!in||!frames||frames>8192||at<consumed||at-consumed>PicassoMixFrames-frames)return -1;
    for(unsigned i=0;i<frames;i++){
        unsigned n=(at+i)%PicassoMixFrames;if(m->tag[n]!=at+i+1){m->sample[n][0]=m->sample[n][1]=0;m->tag[n]=at+i+1;}
        for(unsigned c=0;c<2;c++){float v=in[2*i+c];if(!isfinite(v))v=0;v=v>1?1:v<-1?-1:v;m->sample[n][c]+=v*gain;}
    }return 0;
}
static inline uint64_t picasso_mix_peek(const PicassoAudioMix *m,uint64_t at,unsigned frames,int16_t *out){
    uint64_t missing=0;for(unsigned i=0;i<frames;i++){
        unsigned n=(at+i)%PicassoMixFrames;int valid=m->tag[n]==at+i+1;if(!valid)++missing;
        for(unsigned c=0;c<2;c++){float v=valid?m->sample[n][c]:0;if(!isfinite(v))v=0;v=v>1?1:v<-1?-1:v;out[2*i+c]=(int16_t)(v*32767);}
    }return missing;
}
static inline void picasso_mix_consume(PicassoAudioMix *m,uint64_t at,unsigned frames){for(unsigned i=0;i<frames;i++)m->tag[(at+i)%PicassoMixFrames]=0;}
// Retain only frames the kernel accepted, separately from the consumed mix.
// Exact frame tags prevent replay after wrap, gaps, or restarting a preview.
// All access is protected by the HAL lock. This is not a microphone/amp sensor.
typedef struct {int16_t sample[PicassoMixFrames][2];uint64_t tag[PicassoMixFrames];} PicassoAudioTap;
static inline void picasso_tap_store(PicassoAudioTap *tap,uint64_t at,unsigned frames,const int16_t *in){
    for(unsigned i=0;i<frames;++i){unsigned n=(at+i)%PicassoMixFrames;
        tap->sample[n][0]=in[2*i];tap->sample[n][1]=in[2*i+1];tap->tag[n]=at+i+1;}
}
static inline uint64_t picasso_tap_read(const PicassoAudioTap *tap,uint64_t at,unsigned frames,float *out){
    uint64_t missing=0;
    for(unsigned i=0;i<frames;++i){unsigned n=(at+i)%PicassoMixFrames;int valid=tap->tag[n]==at+i+1;
        if(!valid)++missing;
        for(unsigned c=0;c<2;++c)out[2*i+c]=valid?tap->sample[n][c]/32768.0f:0;
    }return missing;
}
#endif
