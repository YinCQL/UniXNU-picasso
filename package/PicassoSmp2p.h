// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_SMP2P_H
#define PICASSO_SMP2P_H
#include "WlanSmem.h"

// The hardware adapter serializes APPS and holds TCSR mutex 3 across allocation.
// A failed write may have reached memory. Published allocations are never freed.
struct PicassoSmemIo {
    wlan_smem_io reader;
    int (*write32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    bool (*locked)(void *);
};
// Capture under the allocator lock; the adapter logs only after unlocking.
struct PicassoSmemAudit {
    const char *phase;
    uint32_t index, count, offset, size, flags, hosts, cacheline, reserved;
    uint32_t infoMagic, infoSize, infoBaseLow, infoBaseHigh, items;
};
struct PicassoSmp2p {
    struct wlan_smem_partition partition;
    wlan_smem_item out;
    uint32_t last, features, flags;
    bool created, negotiated, failed;
    PicassoSmemAudit audit;
};
struct PicassoSmp2pEvent {
    uint32_t value, changed, features, flags;
    bool restart, kick, hasValue;
};
// Only private APPS/ADSP item 429, freshly allocated; never overwrites an
// existing item or falls back to a different partition owner.
int picasso_smp2p_prepare(const PicassoSmemIo *, PicassoSmp2p *);
// Polled on the serialized DSP worker, outside the remote allocator lock.
int picasso_smp2p_service(const PicassoSmemIo *, PicassoSmp2p *, PicassoSmp2pEvent *);
extern "C" void picasso_smp2p_start(void);
#endif
