// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only Qualcomm SMEM v12 / SMP2P wire-format reader for Picasso.
// Protocol reference: stock drivers/soc/qcom/{smem,smp2p}.c and Raphael
// aa215933e3e4904aa77ee82f1b7b5600d048e5a1 hakios.wifi.c (SMEM survey).
// Independent bounded parser: no remote allocation, locks, mailbox or PAS calls.
#ifndef PICASSO_WLAN_SMEM_H
#define PICASSO_WLAN_SMEM_H
#include <stdint.h>
#include <stddef.h>
#define WLAN_SMEM_BASE 0x80900000ULL
#define WLAN_SMEM_SIZE 0x200000U
// Live Picasso DT qcom,smp2p-mpss qcom,smem=<435 428>, remote-pid=1.
#define WLAN_SMEM_IN 435U
#define WLAN_SMEM_OUT 428U
struct wlan_smem_io { void *context; int (*read)(void *, uint32_t, void *, uint32_t); };
struct wlan_smem_partition { uint32_t offset,size,uncached,cached,cacheline; };
struct wlan_smem_item { uint32_t offset,size; };
static inline uint16_t wlan_sm16(const uint8_t *p) { return p[0] | ((uint16_t)p[1]<<8); }
static inline uint32_t wlan_sm32(const uint8_t *p) { return wlan_sm16(p) | ((uint32_t)wlan_sm16(p+2)<<16); }
static inline int wlan_smread(const struct wlan_smem_io *io,uint32_t off,void *data,uint32_t len) {
    if (!io || !io->read || off>WLAN_SMEM_SIZE || len>WLAN_SMEM_SIZE-off) return -22;
    return io->read(io->context,off,data,len);
}
static inline int wlan_smem_partition(const struct wlan_smem_io *io,uint16_t a,uint16_t b,struct wlan_smem_partition *out) {
    uint8_t h[32],e[48]; const uint32_t toc=WLAN_SMEM_SIZE-4096;
    if (!out || wlan_smread(io,0x5c,h,4) || (wlan_sm32(h)>>16)!=12 || wlan_smread(io,toc,h,32)) return -22;
    uint32_t count=wlan_sm32(h+8);
    if (wlan_sm32(h)!=0x434f5424 || wlan_sm32(h+4)!=1 || count>64) return -22;
    int found=0; struct wlan_smem_partition candidate={0,0,0,0,0};
    for (uint32_t i=0;i<count;++i) {
        if (wlan_smread(io,toc+32+48*i,e,48)) return -22;
        uint16_t x=wlan_sm16(e+12),y=wlan_sm16(e+14);
        if (!((x==a && y==b)||(x==b && y==a))) continue;
        if (found++) return -22;
        uint32_t off=wlan_sm32(e),size=wlan_sm32(e+4),line=wlan_sm32(e+16);
        if (off<0x1000 || (off&7) || size<32 || off>toc || size>toc-off ||
            !line || (line&(line-1)) || line>4096 || wlan_smread(io,off,h,32)) return -22;
        if (wlan_sm32(h)!=0x54525024 || wlan_sm16(h+4)!=x || wlan_sm16(h+6)!=y || wlan_sm32(h+8)!=size) return -22;
        uint32_t uncached=wlan_sm32(h+12),cached=wlan_sm32(h+16);
        if (uncached<32 || (uncached&7) || uncached>cached || cached>size) return -22;
        candidate.offset=off;candidate.size=size;candidate.uncached=uncached;candidate.cached=cached;candidate.cacheline=line;
    }
    if (!found) return -2;
    *out=candidate;return 0;
}
static inline int wlan_smem_find(const struct wlan_smem_io *io,const struct wlan_smem_partition *p,uint16_t id,struct wlan_smem_item *out) {
    uint32_t at=32,visited=0;uint8_t e[16];int found=0;struct wlan_smem_item item={0,0};
    if (!p || !out || p->uncached<32 || p->uncached>p->cached || p->cached>p->size ||
        !p->cacheline || (p->cacheline&(p->cacheline-1)) || p->cacheline>4096 ||
        p->offset>WLAN_SMEM_SIZE || p->size>WLAN_SMEM_SIZE-p->offset) return -22;
    while (at<p->uncached) {
        if (++visited>256 || p->uncached-at<16 || wlan_smread(io,p->offset+at,e,16)) return -22;
        uint32_t size=wlan_sm32(e+4),pad=wlan_sm16(e+8),header=wlan_sm16(e+10);
        if (wlan_sm16(e)!=0xa5a5 || !size || pad>=size || header>p->uncached-at-16 ||
            size>p->uncached-at-16-header) return -22;
        if (wlan_sm16(e+2)==id) {
            if (found++) return -22;
            item.offset=p->offset+at+16+header;item.size=size-pad;
        }
        at+=16+header+size;
        if (at&7) return -22;
    }
    uint32_t stride=(16+p->cacheline-1)&~(p->cacheline-1);
    uint32_t top=p->size;
    while (top>p->cached) {
        if (++visited>256 || top-p->cached<stride) return -22;
        at=top-stride;
        if (wlan_smread(io,p->offset+at,e,16)) return -22;
        uint32_t size=wlan_sm32(e+4),pad=wlan_sm16(e+8);
        if (wlan_sm16(e)!=0xa5a5 || !size || pad>=size || size>at-p->cached || (size&(p->cacheline-1))) return -22;
        if (wlan_sm16(e+2)==id) {
            if (found++) return -22;
            item.offset=p->offset+at-size;item.size=size-pad;
        }
        top=at-size;
    }
    // Detect concurrent remote allocation. Caller can try again on a later boot;
    // never return a partly parsed item or spin on a changing table.
    uint8_t after[20];
    if (wlan_smread(io,p->offset,after,20) || wlan_sm32(after+12)!=p->uncached || wlan_sm32(after+16)!=p->cached) return -11;
    if (!found) return -2;
    *out=item;return 0;
}
static inline int wlan_smp2p_wlan(const struct wlan_smem_io *io,const struct wlan_smem_item *item,uint16_t local,uint16_t remote,uint32_t *value) {
    uint8_t h[20],e[20],after[20];int found=0;uint32_t candidate=0;
    if (!item || !value || item->offset>WLAN_SMEM_SIZE || item->size>WLAN_SMEM_SIZE-item->offset ||
        item->size<20 || wlan_smread(io,item->offset,h,20)) return -22;
    unsigned total=wlan_sm16(h+12),valid=wlan_sm16(h+14);
    if (wlan_sm32(h)!=0x504d5324 || h[4]!=1 || wlan_sm16(h+8)!=local || wlan_sm16(h+10)!=remote ||
        total>16 || valid>total || item->size<20+20*total) return -22;
    for (unsigned i=0;i<valid;++i) {
        if (wlan_smread(io,item->offset+20+20*i,e,20)) return -22;
        if (e[0]=='w' && e[1]=='l' && e[2]=='a' && e[3]=='n' && !e[4]) {
            if (found++) return -22;
            candidate=wlan_sm32(e+16);
        }
    }
    if (wlan_smread(io,item->offset,after,20)) return -22;
    for (unsigned i=0;i<20;++i) if (h[i]!=after[i]) return -11;
    if (!found) return -2;
    *value=candidate;return 0;
}
#endif
