// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito: TCSR mutex 3 @ 0x01f43000, APPS owner=1; SMEM private APPS/ADSP;
// IPCC LPASS client 3 / SMP2P signal 2. No LPASS MMIO or shared IRQ clearing.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/cpu_data.h>
#include "PicassoSmp2p.h"
#include "PicassoSmemLock.h"
#include "PicassoRpmh.h"

// Display also builds without the optional Adreno archive. A missing shared
// owner means no power plan; never create an independent RSC writer.
extern "C" int picasso_rpmh_adsp_plan(void) __attribute__((weak_import));

namespace {
struct SmemDevice { uintptr_t ram, mutex, ipcc; bool held; };
static unsigned allocatorGate, started;
void smemBarrier(void *) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
uint32_t lockRead(void *ctx) { return *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->mutex); }
void lockWrite(void *ctx, uint32_t value) { *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->mutex) = value; }
bool lockHeld(void *ctx) { return static_cast<SmemDevice *>(ctx)->held && lockRead(ctx) == 1; }
int smemRead(void *ctx, uint32_t at, void *out, uint32_t n) {
    if (at > WLAN_SMEM_SIZE || n > WLAN_SMEM_SIZE - at) return -22;
    // Preserve atomic remote 32-bit publication/state words; byte loads could
    // tear a free pointer or ready/fatal value while the peer updates it.
    auto base = static_cast<SmemDevice *>(ctx)->ram;
    for (unsigned i = 0; i < n;) {
        uint32_t address = at + i, shift = address & 3;
        uint32_t word = *reinterpret_cast<volatile uint32_t *>(base + (address & ~3U));
        for (; shift < 4 && i < n; ++shift, ++i)
            static_cast<uint8_t *>(out)[i] = uint8_t(word >> (8 * shift));
    }
    smemBarrier(ctx); return 0;
}
int smemWrite(void *ctx, uint32_t at, uint32_t value) {
    if ((at & 3) || at > WLAN_SMEM_SIZE - 4) return -22;
    *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->ram + at) = value;
    return 0;
}
void smpKick(SmemDevice *d) {
    smemBarrier(d);
    *reinterpret_cast<volatile uint32_t *>(d->ipcc + 0x0c) = (3U << 16) | 2;
    smemBarrier(d);
}
IOMemoryMap *smemMap(uint64_t base, uint64_t bytes) {
    IODeviceMemory *m = IODeviceMemory::withRange(base, bytes);
    if (!m) return nullptr;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release(); return map;
}
int prepare(SmemDevice *d, const PicassoSmemIo *io, PicassoSmp2p *s) {
    s->audit.phase = "lock";
    if (__atomic_exchange_n(&allocatorGate, 1U, __ATOMIC_ACQ_REL)) return -16;
    PicassoSmemLockIo lock = {d, lockRead, lockWrite, smemBarrier};
    int error = -60;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        // No scheduling while APPS owns the cross-processor allocator lock.
        disable_preemption();
        int acquired = picasso_smem_trylock(&lock);
        if (!acquired) {
            d->held = true;
            error = picasso_smp2p_prepare(io, s);
            int released = picasso_smem_unlock(&lock);
            d->held = false;
            enable_preemption();
            if (released) error = released;
            break;
        }
        enable_preemption();
        if (acquired != -11) { error = acquired; break; }
        IOSleep(1);
    }
    __atomic_store_n(&allocatorGate, 0U, __ATOMIC_RELEASE);
    return error;
}
void logAudit(const PicassoSmp2p &s) {
    const auto &a = s.audit;
    IOLog("PICASSO_ADSP smem phase=%s entry=%u/%u offset=%x size=%x flags=%x hosts=%08x line=%x reserved=%x free=%x/%x siii=%x region=%x base=%08x%08x items=%u\n",
        a.phase ? a.phase : "none", a.index, a.count, a.offset, a.size, a.flags, a.hosts,
        a.cacheline, a.reserved, s.partition.uncached, s.partition.cached,
        a.infoMagic, a.infoSize, a.infoBaseHigh, a.infoBaseLow, a.items);
}
}

extern "C" void picasso_smp2p_start(void) {
    // The existing firmware worker becomes the sole SMP2P service thread.
    if (__atomic_exchange_n(&started, 1U, __ATOMIC_ACQ_REL)) return;
    // Independent, read-only command DB discovery, including SMEM failures.
    // This does not acquire the allocator lock or send DSP power votes.
    if (picasso_rpmh_adsp_plan) (void)picasso_rpmh_adsp_plan();
    else IOLog("PICASSO_ADSP rpmh stage=plan error=-6 vote_sent=0 pending=shared-owner\n");
    OSSharedPtr<IOMemoryMap> ram(smemMap(WLAN_SMEM_BASE, WLAN_SMEM_SIZE), OSNoRetain);
    OSSharedPtr<IOMemoryMap> mutex(smemMap(0x01f43000, 0x1000), OSNoRetain);
    OSSharedPtr<IOMemoryMap> ipcc(smemMap(0x00408000, 0x1000), OSNoRetain);
    SmemDevice d = {ram ? uintptr_t(ram->getVirtualAddress()) : 0,
                   mutex ? uintptr_t(mutex->getVirtualAddress()) : 0,
                   ipcc ? uintptr_t(ipcc->getVirtualAddress()) : 0, false};
    if (!d.ram || !d.mutex || !d.ipcc) {
        IOLog("PICASSO_ADSP smp2p stage=map error=-12\n"); return;
    }
    PicassoSmemIo io = {{&d, smemRead}, smemWrite, smemBarrier, lockHeld};
    PicassoSmp2p s = {};
    int error = prepare(&d, &io, &s);
    IOLog("PICASSO_ADSP smp2p stage=prepare error=%d created=%u partition=%x/%x out=%x/%x owner=%x irq=0 authenticated=0\n",
        error, s.created, s.partition.offset, s.partition.size, s.out.offset, s.out.size, lockRead(&d));
    logAudit(s);
    if (error) return;
    smpKick(&d);
    unsigned missing = 0, logs = 0;
    bool wasOpen = false, hadValue = false;
    for (;;) {
        PicassoSmp2pEvent event = {};
        error = picasso_smp2p_service(&io, &s, &event);
        if (error && error != -2 && error != -11) {
            IOLog("PICASSO_ADSP smp2p stage=peer error=%d stopped=1 authenticated=0\n", error);
            logAudit(s);
            return; // Preserve the published item; no blind teardown or reuse.
        }
        if (!error) {
            if (event.kick) smpKick(&d);
            if ((!wasOpen || event.hasValue != hadValue || event.changed || event.restart) && logs < 64) {
                ++logs;
                IOLog("PICASSO_ADSP smp2p peer=1 entry=%u value=%x changed=%x features=%x restart=%u ready=%u fatal=%u handover=%u stop_ack=%u authenticated=0 pcm=0\n",
                    event.hasValue, event.value, event.changed, event.features, event.restart,
                    !!(event.value & 2), !!(event.value & 1), !!(event.value & 4), !!(event.value & 8));
            }
            wasOpen = true;
            hadValue = event.hasValue;
        } else if (missing == 0 || missing == 250 || missing == 1500) {
            IOLog("PICASSO_ADSP smp2p peer=0 error=%d elapsed_ms=%u pending=power-pas authenticated=0\n", error, missing * 20);
        }
        if (missing < 1501) ++missing;
        IOSleep(20);
    }
}
