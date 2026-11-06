// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso stock lito.dtsi/lito-smp2p.dtsi and qcom_glink_smem.c wire facts.
// Read-only observations do not prove PAS authentication or channel ownership.
#ifndef PICASSO_ADSP_H
#define PICASSO_ADSP_H
#include "WlanSmem.h"
#define PICASSO_ADSP_BASE 0x89200000ULL
#define PICASSO_ADSP_SIZE 0x02800000U
#define PICASSO_ADSP_PAS 1U
struct picasso_adsp_snapshot {
    struct wlan_smem_partition partition;
    int partition_error, inbound_error, outbound_error, glink_error;
    uint32_t inbound, outbound, fifo[4];
    int crash_error;
    char crash[256]; // stock MAX_SSR_REASON_LEN, including the terminator
};
static inline int picasso_adsp_item(const struct wlan_smem_io *io,
    const struct wlan_smem_partition *p,uint16_t id,uint16_t local,uint16_t remote,
    const char *name,uint32_t *value) {
    struct wlan_smem_item item={0,0};
    int error=wlan_smem_find(io,p,id,&item);
    return error ? error : wlan_smp2p_entry(io,&item,local,remote,name,value);
}
static inline int picasso_adsp_glink(const struct wlan_smem_io *io,
    const struct wlan_smem_partition *p,uint32_t fifo[4]) {
    struct wlan_smem_item items[3]={{0,0},{0,0},{0,0}};
    for (unsigned i=0;i<3;++i) {
        int error=wlan_smem_find(io,p,(uint16_t)(478+i),&items[i]);
        if (error) return error;
        if (i<2 ? items[i].size!=(i ? 16384U : 32U) :
            (items[i].size<16 || items[i].size>65536 || (items[i].size&7))) return -22;
    }
    uint8_t first[16],after[16];
    if (wlan_smread(io,items[0].offset,first,16) ||
        wlan_smread(io,items[0].offset,after,16)) return -22;
    for (unsigned i=0;i<16;++i) if (first[i]!=after[i]) return -11;
    for (unsigned i=0;i<4;++i) {
        uint32_t v=wlan_sm32(first+4*i);
        if (v>=items[i<2 ? 1 : 2].size || (v&7)) return -22;
    }
    for (unsigned i=0;i<4;++i) fifo[i]=wlan_sm32(first+4*i);
    return 0;
}
static inline int picasso_adsp_crash(const struct wlan_smem_io *io,char text[256]) {
    if (!text) return -22;
    text[0]=0;
    struct wlan_smem_partition p={0,0,0,0,0};
    struct wlan_smem_item item={0,0};
    int error=wlan_smem_partition(io,0xfffe,0xfffe,&p);
    if (!error) error=wlan_smem_find(io,&p,423,&item);
    if (error) return error;
    if (!item.size) return -22;
    uint8_t first[255],after[255];
    uint32_t size=item.size<255 ? item.size : 255;
    if (wlan_smread(io,item.offset,first,size) || wlan_smread(io,item.offset,after,size)) return -22;
    for (uint32_t i=0;i<size;++i) if (first[i]!=after[i]) return -11;
    uint32_t n=0;
    for (;n<size && first[n];++n) text[n]=first[n]>=32 && first[n]<127 ? (char)first[n] : '.';
    text[n]=0;
    return 0;
}
static inline void picasso_adsp_survey(const struct wlan_smem_io *io,struct picasso_adsp_snapshot *s) {
    *s=(struct picasso_adsp_snapshot){{0,0,0,0,0},0,-2,-2,-2,0,0,{0,0,0,0},-2,{0}};
    // Crash SMEM is global even when a private APPS/ADSP partition exists.
    s->crash_error=picasso_adsp_crash(io,s->crash);
    s->partition_error=wlan_smem_partition(io,0,2,&s->partition);
    // Stock qcom_smem_get falls back only when the private partition is absent.
    if (s->partition_error==-2) s->partition_error=wlan_smem_partition(io,0xfffe,0xfffe,&s->partition);
    if (s->partition_error) return;
    s->inbound_error=picasso_adsp_item(io,&s->partition,443,2,0,"slave-kernel",&s->inbound);
    s->outbound_error=picasso_adsp_item(io,&s->partition,429,0,2,"master-kernel",&s->outbound);
    s->glink_error=picasso_adsp_glink(io,&s->partition,s->fifo);
}
#endif
