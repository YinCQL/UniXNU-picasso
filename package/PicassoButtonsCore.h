// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7250 / SPMI v5, from lito.dtsi and spmi-pmic-arb.c. No reset writes.
#pragma once
#include <stdint.h>
namespace picasso_buttons {
constexpr uint32_t apids = 512, map_offset = 0x900, owner_offset = 0x700;
constexpr uint32_t pon_ppid = 0x008, up_ppid = 0x4c4;
constexpr uint32_t report_bits = 0x0e;
inline bool version_supported(uint32_t version) { return version >= 0x50000000 && version < 0x60000000; }
inline uint32_t observer_offset(unsigned apid) { return apid * 0x80; } // EE 0
inline uint32_t command(uint8_t reg) { return (1u << 27) | (uint32_t(reg) << 4); }

// Use only a mapping assigned to APPS EE 0; never assume APID == PPID.
template<class Read> int find_apid(Read read, uint32_t ppid) {
    int found = -1;
    for (unsigned i = 0; i < apids; ++i) {
        uint32_t map = read(false, map_offset + i * 4);
        if (map && ((map >> 8) & 0xfff) == ppid &&
            (read(true, owner_offset + i * 4) & 7) == 0) found = int(i);
    }
    return found;
}

// read/write operate on one observer channel. Status is checked after issuing
// the command, as in the vendor driver; a timeout poisons the session upstream.
template<class Bus> int read8(Bus &bus, uint8_t reg, uint8_t &value) {
    bus.write(0, command(reg));
    for (unsigned n = 0; n < 1000; ++n) {
        uint32_t status = bus.read(8);
        if (status & 1) {
            if (status & 0xe) return int(status & 0xe);
            value = uint8_t(bus.read(0x18));
            return 0;
        }
        bus.delay();
    }
    return -1;
}
inline bool gpio_input(uint8_t type, uint8_t subtype, uint8_t mode, uint8_t enable) {
    if (type != 0x10 || !(enable & 0x80)) return false;
    switch (subtype) {
    case 0x10: case 0x11: return !(mode & 3);
    case 1: case 5: case 9: case 0xd: return !(mode & 0x70);
    default: return false;
    }
}
inline uint32_t mask(uint8_t pon, uint8_t gpio) {
    return ((pon & 1) ? 2u : 0u) | ((pon & 2) ? 8u : 0u) | ((gpio & 1) ? 0u : 4u);
}

struct Debounce {
    uint32_t stable = 0, candidate = 0;
    uint64_t since[3] = {};
    // Independent per key: bouncing volume must not suppress a power release.
    bool sample(uint32_t raw, uint64_t now) {
        uint32_t before = stable;
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t bit = 2u << i;
            if ((raw ^ candidate) & bit) { candidate ^= bit; since[i] = now; }
            if (now - since[i] >= 20) stable = (stable & ~bit) | (candidate & bit);
        }
        return stable != before;
    }
};

struct Reports {
    Debounce debounce;
    uint32_t delivered = 0;
    // Retry a failed submission even when the next hardware sample is unchanged.
    // The Consumer report is a state bitmap: a held key is never toggled/repeated.
    template<class Send> bool sample(uint32_t raw, uint64_t now, Send send) {
        debounce.sample(raw, now);
        if (debounce.stable == delivered || !send(debounce.stable)) return false;
        delivered = debounce.stable;
        return true;
    }
};
}
