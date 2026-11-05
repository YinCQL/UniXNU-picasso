// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM8150L WLED5 module gating, as specified by the stock qcom-spmi-wled
// wled_module_enable sequence. Adopt the firmware's brightness/CABC settings.
#pragma once
#include <stdint.h>
namespace picasso_backlight {
constexpr unsigned control_ppid = 0x5d8, sink_ppid = 0x5d9;
// Readback mismatch is distinct from an SPMI transport failure.
constexpr int mismatch = -2, configuration = -3;
template<class Bus> int checked_write(Bus &bus, bool sink, uint8_t reg, uint8_t value) {
    int error = bus.write(sink, reg, value);
    uint8_t actual = 0;
    if (!error) error = bus.read(sink, reg, actual);
    return error ? error : actual == value ? 0 : mismatch;
}
template<class Read> int find_apid(Read read, unsigned ppid) {
    int found = -1;
    for (unsigned i=0;i<512;++i) {
        uint32_t map=read(false,0x900+i*4);
        if (map && ((map>>8)&0xfff)==ppid && (read(true,0x700+i*4)&7)==0) {
            if (found!=-1) return -2;
            found=int(i);
        }
    }
    return found;
}
struct State {
    bool ready = false, on = false, failed = false;
    uint8_t module = 0;
    // No brightness/current/voltage/IRQ changes during adoption. The shipped
    // g7a DT selects MOD_A, 12-bit brightness, CABC1, strings 0..2.
    template<class Bus> int adopt(Bus &bus) {
        const struct { bool sink; uint8_t reg, mask, value; } checks[] = {
            {false,0x08,0x07,0}, {false,0x46,0x80,0x80},
            {true,0x46,0xf0,0x70}, {true,0x50,0x80,0x80},
            {true,0x51,0x03,1}, {true,0x52,0x01,0},
            {true,0x73,0x01,0}, {true,0x83,0x01,0}, {true,0x93,0x01,0},
            {true,0xb0,0xc0,0},
        };
        for (const auto &check : checks) {
            uint8_t value = 0;
            int error = bus.read(check.sink, check.reg, value);
            if (error) return error;
            if ((value & check.mask) != check.value) return configuration;
            if (!check.sink && check.reg == 0x46) module = value;
        }
        uint8_t lo = 0, hi = 0;
        int error = bus.read(true, 0x53, lo);
        if (!error) error = bus.read(true, 0x54, hi);
        if (error) return error;
        if ((hi & 0xf0) || !(lo | hi)) return configuration;
        ready = on = true;
        return 0;
    }
    template<class Bus> int set(Bus &bus, bool requested) {
        if (!ready || failed) return configuration;
        uint8_t actual = 0, faults = 0, flash = 0;
        int error = bus.read(false, 0x46, actual);
        if (!error) error = bus.read(false, 0x08, faults);
        if (!error) error = bus.read(true, 0xb0, flash);
        if (!error && (actual != uint8_t((module & 0x7f) | (on ? 0x80 : 0)) ||
                       (faults & 7) || (flash & 0xc0))) error = configuration;
        if (!error && requested == on) return 0;
        // HFRC off before module gate; clear flash shutdown on enable, then
        // allow the documented soft-start interval. No PMIC IRQ is claimed.
        if (!error) error = checked_write(bus, false, 0xef, requested ? 0 : 3);
        if (!error) error = checked_write(bus, false, 0x46,
            uint8_t((module & 0x7f) | (requested ? 0x80 : 0)));
        if (!error && requested) error = checked_write(bus, true, 0xb6, 0);
        if (!error && requested) {
            bus.soft_start();
            error = bus.read(false, 0x08, faults);
            if (!error && (faults & 7)) error = configuration;
        }
        // A partial write never counts as completion. Stop this session;
        // subsequent calls must not reuse a timed-out transaction.
        if (error) failed = true;
        else on = requested;
        return error;
    }
};
}
