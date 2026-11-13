// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_SNAPSHOT_RESAMPLE_CORE_H
#define PICASSO_SNAPSHOT_RESAMPLE_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

struct PicassoSnapshotAxis { uint16_t first,second; uint32_t weight; };
static struct PicassoSnapshotAxis picasso_snapshot_axis(size_t index,size_t source,size_t target)
{
    int64_t position=(int64_t)(((2*index+1)*(uint64_t)source*65536)/(2*target))-32768;
    if(position<0)position=0;
    int64_t last=(int64_t)(source-1)*65536;
    if(position>last)position=last;
    size_t first=(size_t)(position>>16);
    return (struct PicassoSnapshotAxis){first,first+1<source?first+1:first,(uint32_t)position&65535};
}

// Source and destination are distinct 8-bit BGRA buffers. The caller supplies
// opaque or premultiplied pixels and owns both buffers for the whole operation.
// Pixel centers map to pixel centers; clamp at the visible edge, never padding.
static bool picasso_snapshot_resample(const unsigned char *source,size_t sw,size_t sh,size_t sr,
    unsigned char *target,size_t tw,size_t th,size_t tr)
{
    if(!source||!target||source==target||!sw||!sh||!tw||!th||sw>4096||sh>4096||tw>sw||th>sh||sr<sw*4||tr<tw*4||sr>SIZE_MAX/sh||tr>SIZE_MAX/th)return false;
    size_t sn=sr*sh,tn=tr*th;
    uintptr_t sa=(uintptr_t)source,ta=(uintptr_t)target;
    if(sn>UINTPTR_MAX-sa||tn>UINTPTR_MAX-ta||(sa<ta+tn&&ta<sa+sn))return false;
    struct PicassoSnapshotAxis *xs=malloc(tw*sizeof(*xs));
    if(!xs)return false;
    for(size_t x=0;x<tw;x++)xs[x]=picasso_snapshot_axis(x,sw,tw);
    for(size_t y=0;y<th;y++){
        struct PicassoSnapshotAxis ay=picasso_snapshot_axis(y,sh,th);
        const unsigned char *row0=source+ay.first*sr,*row1=source+ay.second*sr;
        unsigned char *out=target+y*tr;
        for(size_t x=0;x<tw;x++){
            struct PicassoSnapshotAxis ax=xs[x];
            for(unsigned c=0;c<4;c++){
                uint32_t a=row0[ax.first*4+c]*(65536-ax.weight)+row0[ax.second*4+c]*ax.weight;
                uint32_t b=row1[ax.first*4+c]*(65536-ax.weight)+row1[ax.second*4+c]*ax.weight;
                uint64_t value=(uint64_t)a*(65536-ay.weight)+(uint64_t)b*ay.weight;
                out[4*x+c]=(unsigned char)((value+0x80000000ull)>>32);
            }
        }
    }free(xs);return true;
}

// BGRX has no alpha semantics. Ignore the source's unused byte and publish
// opaque alpha after filtering the color channels, matching an opaque context.
static bool picasso_snapshot_resample_opaque(const unsigned char *source,size_t sw,size_t sh,size_t sr,
    unsigned char *target,size_t tw,size_t th,size_t tr)
{
    if(!picasso_snapshot_resample(source,sw,sh,sr,target,tw,th,tr))return false;
    for(size_t y=0;y<th;y++)for(size_t x=0;x<tw;x++)target[y*tr+x*4+3]=255;
    return true;
}

#endif
