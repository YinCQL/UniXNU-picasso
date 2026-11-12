// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "PicassoSpmi.h"
#include "PicassoSpmiCore.h"
namespace {
IOSimpleLock *controllerLock;
bool controllerStopped;
struct Channel {
    volatile uint32_t *va;
    IOSimpleLock *mutex;
    IOInterruptState lock() { return IOSimpleLockLockDisableInterrupt(mutex); }
    void unlock(IOInterruptState irq) { IOSimpleLockUnlockEnableInterrupt(mutex,irq); }
    void write(unsigned offset,uint32_t value) {
        va[offset/4]=value; __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t read(unsigned offset) {
        uint32_t value=va[offset/4]; __asm__ volatile("dsb sy" ::: "memory"); return value;
    }
    void delay() { IODelay(1); }
};
}
extern "C" bool picasso_spmi_init() {
    if (__atomic_load_n(&controllerLock,__ATOMIC_ACQUIRE)) return true;
    IOSimpleLock *next=IOSimpleLockAlloc();
    if (!next) return false;
    IOSimpleLock *expected=nullptr;
    if (!__atomic_compare_exchange_n(&controllerLock,&expected,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        IOSimpleLockFree(next);
    return true;
}
extern "C" int picasso_spmi_read8(volatile uint32_t *va,uint8_t reg,uint8_t &value) {
    IOSimpleLock *mutex=__atomic_load_n(&controllerLock,__ATOMIC_ACQUIRE);
    if (!mutex || !va) return picasso_spmi::unavailable;
    Channel channel{va,mutex};
    unsigned retries=0;
    int error=picasso_spmi::read8(channel,reg,value,retries,&controllerStopped);
    // No allocation, sleeping or logging while the IRQ-disabling lock is held.
    if (error || retries) {
        static unsigned reports;
        unsigned ticket=__atomic_fetch_add(&reports,1U,__ATOMIC_RELAXED);
        if (ticket<32) IOLog("PICASSO_SPMI read reg=%02x error=%d dropped_retries=%u\n",reg,error,retries);
    }
    return error;
}
extern "C" int picasso_spmi_write8(volatile uint32_t *va,uint8_t reg,uint8_t value) {
    IOSimpleLock *mutex=__atomic_load_n(&controllerLock,__ATOMIC_ACQUIRE);
    if (!mutex || !va) return picasso_spmi::unavailable;
    Channel channel{va,mutex};
    return picasso_spmi::write8(channel,reg,value,&controllerStopped);
}

// Drain the last transaction under the same lock, then refuse all later
// commands. Used only at final power-off, before secure arbiter shutdown.
extern "C" int picasso_spmi_quiesce() {
    IOSimpleLock *mutex=__atomic_load_n(&controllerLock,__ATOMIC_ACQUIRE);
    if(!mutex)return picasso_spmi::unavailable;
    IOInterruptState irq=IOSimpleLockLockDisableInterrupt(mutex);
    __atomic_store_n(&controllerStopped,true,__ATOMIC_RELEASE);
    IOSimpleLockUnlockEnableInterrupt(mutex,irq);
    return 0;
}
