// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_CAPTURE_DMA_H
#define PICASSO_CAPTURE_DMA_H
#include "PicassoCapture.h"
enum { PicassoCapturePoolBytes=32768, PicassoCaptureSlotBytes=4096 };
static constexpr uint64_t PicassoCaptureAddress=0x180008000ULL;
inline uint64_t picasso_capture_dma_leaf(uint64_t address) {
    return address|3|(1U<<6)|(2U<<8)|(1U<<10)|(1U<<11)|(3ULL<<53);
}
inline int picasso_capture_dma_geometry(uint64_t capture,uint64_t playback) {
    if(!capture || !playback || (capture&4095) || (playback&4095) ||
       capture>=(1ULL<<36) || playback>=(1ULL<<36) ||
       PicassoCapturePoolBytes>(1ULL<<36)-capture || 45056>(1ULL<<36)-playback)return -22;
    // The capture allocation must not alias playback data or translation pages.
    if(capture<playback+45056 && playback<capture+PicassoCapturePoolBytes)return -16;
    return 0;
}
inline int picasso_capture_dma_verify(const uint64_t *tables,uint64_t capture,uint64_t playback) {
    int e=picasso_capture_dma_geometry(capture,playback);if(e || !tables || (uintptr_t(tables)&7))return e?e:-22;
    if(tables[2]!=((playback+36864)|3) || tables[512]!=((playback+40960)|3))return -5;
    for(unsigned i=0;i<8;++i)
        if(tables[1024+i]!=picasso_capture_dma_leaf(playback+4096*i) ||
           tables[1032+i]!=picasso_capture_dma_leaf(capture+4096*i))return -5;
    return 0;
}
inline int picasso_capture_dma_add(uint64_t *tables,uint64_t capture,uint64_t playback) {
    int e=picasso_capture_dma_geometry(capture,playback);if(e || !tables || (uintptr_t(tables)&7))return e?e:-22;
    if(tables[2]!=((playback+36864)|3) || tables[512]!=((playback+40960)|3))return -5;
    for(unsigned i=0;i<8;++i)
        if(tables[1024+i]!=picasso_capture_dma_leaf(playback+4096*i) || tables[1032+i])return -16;
    for(unsigned i=0;i<8;++i)tables[1032+i]=picasso_capture_dma_leaf(capture+4096*i);
    return picasso_capture_dma_verify(tables,capture,playback);
}
#endif
