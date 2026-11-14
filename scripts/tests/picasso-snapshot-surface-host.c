// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production surface transaction with controlled IOSurface APIs.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "picasso-snapshot-resample-core.h"

typedef int kern_return_t;
enum { kIOSurfaceLockReadOnly = 1 };
typedef struct FakeSurface {
    size_t width, height, row, allocation, planes, element;
    uint32_t format, seed;
    unsigned char *bytes;
    int locks, unlocks, lockFailure, unlockFailure, changeAtUnlock, nullBase;
} *IOSurfaceRef;
static size_t IOSurfaceGetWidth(IOSurfaceRef s) { return s->width; }
static size_t IOSurfaceGetHeight(IOSurfaceRef s) { return s->height; }
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) { return s->row; }
static size_t IOSurfaceGetAllocSize(IOSurfaceRef s) { return s->allocation; }
static size_t IOSurfaceGetPlaneCount(IOSurfaceRef s) { return s->planes; }
static size_t IOSurfaceGetBytesPerElement(IOSurfaceRef s) { return s->element; }
static uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s) { return s->format; }
static uint32_t IOSurfaceGetSeed(IOSurfaceRef s) { return s->seed; }
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s) { return s->nullBase ? NULL : s->bytes; }
static kern_return_t IOSurfaceLock(IOSurfaceRef s, unsigned flags, uint32_t *seed)
{
    if (s->lockFailure) return 1;
    assert(s->locks == s->unlocks);
    ++s->locks;
    if (!flags) ++s->seed;
    if (seed) *seed = s->seed;
    return 0;
}
static kern_return_t IOSurfaceUnlock(IOSurfaceRef s, unsigned flags, uint32_t *seed)
{
    (void)flags;
    assert(s->locks == s->unlocks + 1);
    ++s->unlocks;
    if (s->changeAtUnlock) ++s->seed;
    if (seed) *seed = s->seed;
    return s->unlockFailure;
}
#include "picasso-snapshot-surface.h"

static struct FakeSurface surface(size_t width, size_t height, size_t row)
{
    struct FakeSurface s = {.width=width,.height=height,.row=row,.allocation=row*height,
        .element=4,.format=0x42475241,.seed=1};
    s.bytes=malloc(s.allocation);assert(s.bytes);
    return s;
}
int main(void)
{
    struct FakeSurface source=surface(1080,2400,4352),target=surface(756,1680,3072);
    memset(source.bytes,255,source.allocation);memset(target.bytes,0xa5,target.allocation);
    source.bytes[3]=0; // Unused BGRX byte, not transparent content.
    assert(!picasso_snapshot_resample_surface(&source,&target,false));
    assert(target.locks==0);
    for(size_t n=0;n<target.allocation;n++)assert(target.bytes[n]==0xa5);
    assert(picasso_snapshot_resample_surface(&source,&target,true));
    assert(source.bytes[3]==0&&target.seed==2);
    for(size_t y=0;y<target.height;y++)for(size_t x=0;x<target.row;x++)
        assert(target.bytes[y*target.row+x]==(x<756*4?255:0xa5));
    for(unsigned failure=0;failure<8;failure++){
        memset(target.bytes,0xa5,target.allocation);
        source.lockFailure=failure==0;source.unlockFailure=failure==1;
        source.changeAtUnlock=failure==2;source.nullBase=failure==3;
        target.lockFailure=failure==4;target.nullBase=failure==5;
        source.format=failure==6?0:0x42475241;target.allocation=failure==7?32:3072*1680;
        assert(!picasso_snapshot_resample_surface(&source,&target,true));
        assert(source.locks==source.unlocks&&target.locks==target.unlocks);
        for(size_t n=0;n<3072*1680;n++)assert(target.bytes[n]==0xa5);
    }
    assert(!picasso_snapshot_resample_surface(&source,&source,true));
    free(source.bytes);free(target.bytes);
    puts("PASS production surface transaction: BGRX, publication, padding, unsupported format/size, lock/base failures and seed race");
}
