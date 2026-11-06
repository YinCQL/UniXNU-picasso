// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso/lito AOP: 4 KiB message RAM, IPCC client 0 / signal 0.
// Run only on the firmware worker. Poll shared state without claiming the
// shared IPCC receive IRQ or touching another subsystem's pending signals.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include "PicassoQmp.h"

namespace {
struct Aop { uintptr_t ram, ipcc; };
uint32_t aopRead(void *ctx, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ram + offset);
}
void aopWrite(void *ctx, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ram + offset) = value;
}
void aopBarrier(void *) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
void aopKick(void *ctx) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ipcc + 0x0c) = 0;
    aopBarrier(ctx);
}
IOMemoryMap *aopMap(uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, 0x1000);
    if (!memory) return nullptr;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    return map;
}
uint64_t deadline() {
    uint64_t value; clock_interval_to_deadline(5, kSecondScale, &value); return value;
}
int wait(PicassoQmp *q) {
    for (;;) {
        int state = picasso_qmp_step(q, mach_absolute_time());
        if (state <= QmpClosed || state == QmpOpen) return state;
        IOSleep(1);
    }
}
}

extern "C" int picasso_aop_probe(void) {
    // One attempt per boot. A timeout cannot cause a second owner to take over.
    static unsigned attempted;
    if (__atomic_exchange_n(&attempted, 1U, __ATOMIC_ACQ_REL)) return QmpBusy;
    OSSharedPtr<IOMemoryMap> ram(aopMap(0x0c300000), OSNoRetain);
    OSSharedPtr<IOMemoryMap> ipcc(aopMap(0x00408000), OSNoRetain);
    Aop context = {ram ? static_cast<uintptr_t>(ram->getVirtualAddress()) : 0,
                   ipcc ? static_cast<uintptr_t>(ipcc->getVirtualAddress()) : 0};
    if (!context.ram || !context.ipcc) {
        IOLog("PICASSO_ADSP aop stage=map error=-12 authenticated=0\n"); return -12;
    }
    QmpIo io = {&context, aopRead, aopWrite, aopBarrier, aopKick};
    PicassoQmp q = {};
    int result = picasso_qmp_begin(&q, &io, 0x1000, deadline());
    if (result >= 0) result = wait(&q);
    const uint32_t *d = q.descriptor;
    IOLog("PICASSO_ADSP aop stage=handshake state=%d open=%u magic=%x version=%x features=%x rx=%x/%x tx=%x/%x remote=%x/%x/%x/%x local=%x/%x/%x/%x authenticated=0\n",
        result, result == QmpOpen, d[0], d[1], d[2], d[8], d[7], d[14], d[13],
        d[3], d[4], d[5], d[6], d[9], d[10], d[11], d[12]);
    int closed = picasso_qmp_close(&q, deadline());
    if (closed == QmpClosing) closed = wait(&q);
    IOLog("PICASSO_ADSP aop stage=close state=%d released=%u load_state_sent=0 pending=rails-smp2p-pas pcm=0\n",
        closed, !q.owned);
    // Mapping lifetime ends here. No load_state on/off is sent before a real
    // PAS lifecycle owns LCX/LMX/XO and has installed ready/fatal handlers.
    return result == QmpOpen && closed == QmpClosed ? 0 : (result < 0 ? result : closed);
}
