// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7250 SPMI v5 transaction discipline from stock spmi-pmic-arb.c:
// one controller lock covers CMD through DONE/status and RDATA, across APIDs.
#pragma once
#include <stdint.h>
namespace picasso_spmi {
constexpr int timeout = -1, unavailable = -4;
constexpr uint32_t done = 1, failure = 2, denied = 4, dropped = 8;
template<class Bus> int completion(Bus &bus) {
    for (unsigned n=0;n<1000;++n) {
        uint32_t status=bus.read(8);
        if (status & done) return int(status & (failure|denied|dropped));
        bus.delay();
    }
    return timeout;
}
template<class Bus> int read8(Bus &bus, uint8_t reg, uint8_t &value, unsigned &retries, const bool *stopped=nullptr) {
    retries=0;
    for (;;) {
        auto irq=bus.lock();
        if (stopped && __atomic_load_n(stopped,__ATOMIC_ACQUIRE)) {bus.unlock(irq);return unavailable;}
        bus.write(0,(1U<<27)|(uint32_t(reg)<<4));
        int error=completion(bus);
        if (!error) value=uint8_t(bus.read(0x18));
        bus.unlock(irq);
        // Only DONE|DROPPED: this read completed without returning data or
        // modifying the peripheral. Retry at most twice, outside the lock.
        // Never reuse a timed-out channel or retry DENIED/FAILURE/writes.
        if (error!=int(dropped) || retries==2) return error;
        ++retries;
        bus.delay();
    }
}
template<class Bus> int write8(Bus &bus, uint8_t reg, uint8_t value, const bool *stopped=nullptr) {
    auto irq=bus.lock();
    if (stopped && __atomic_load_n(stopped,__ATOMIC_ACQUIRE)) {bus.unlock(irq);return unavailable;}
    bus.write(0x10,value);
    bus.write(0,uint32_t(reg)<<4);
    int error=completion(bus);
    bus.unlock(irq);
    return error;
}
}
