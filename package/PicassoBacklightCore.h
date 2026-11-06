// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM8150L WLED5 module gating, as specified by the stock qcom-spmi-wled
// wled_module_enable and wled5_set_brightness sequences. Preserve firmware CABC.
#pragma once
#include <stdint.h>
namespace picasso_backlight {
constexpr unsigned control_ppid = 0x5d8, sink_ppid = 0x5d9;
// Readback mismatch is distinct from an SPMI transport failure.
constexpr int mismatch = -2, configuration = -3;
// WLED5 HIGH below/equal 1% needs revision-specific sample/hold changes on
// enable (stock wled5_sample_hold_control). Keep that unqualified path out.
constexpr uint32_t min_level = 42, max_level = 4095;
inline uint32_t clamp(uint32_t n) { return n ? n<min_level?min_level:n>max_level?max_level:n : 0; }
// Decode a finite IEEE binary64 normalized value with integer arithmetic only.
// Reject negative (except -0), NaN, infinity and >1; round to the nearest level.
inline bool normalized(uint64_t ieee, uint32_t *level) {
    uint64_t magnitude = ieee & 0x7fffffffffffffffULL;
    if ((ieee >> 63) && magnitude) return false;
    if (magnitude > 0x3ff0000000000000ULL) return false;
    unsigned exponent = unsigned(magnitude >> 52);
    if (exponent < 1010) { *level = 0; return true; }
    unsigned shift = 1075 - exponent; // [52,65]; product needs 65 bits.
    uint64_t significand = (magnitude & 0xfffffffffffffULL) | (1ULL << 52);
    unsigned __int128 scaled = (unsigned __int128)significand * max_level;
    *level = uint32_t((scaled + ((unsigned __int128)1 << (shift - 1))) >> shift);
    return true;
}
struct Fade {
    uint32_t from=0,to=0,duration=0;uint64_t start=0;
    void begin(uint32_t current,uint32_t target,uint32_t ms,uint64_t now) {
        from=current;to=clamp(target);duration=ms>5000?5000:ms;start=now;
    }
    uint32_t value(uint64_t now) const {
        uint64_t elapsed=now>=start?now-start:0;
        if(!duration || elapsed>=duration)return to;
        return clamp(uint32_t(int64_t(from)+(int64_t(to)-from)*int64_t(elapsed)/duration));
    }
    bool active(uint64_t now) const {return duration && now>=start && now-start<duration;}
};
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
    uint8_t module = 0, source = 0;
    uint32_t level = 0;
    // No brightness/current/voltage/IRQ changes during adoption. The shipped
    // g7a DT selects MOD_A, 12-bit brightness, strings 0..2. Boot160 uses HIGH, not DT CABC1.
    template<class Bus> int adopt(Bus &bus) {
        const struct { bool sink; uint8_t reg, mask, value; } checks[] = {
            {false,0x08,0x07,0}, {false,0x46,0x80,0x80},
            {true,0x46,0xf0,0x70}, {true,0x50,0x80,0x80},
            {true,0x52,0x01,0},
            {true,0x73,0x03,0}, {true,0x83,0x03,0}, {true,0x93,0x03,0},
            {true,0xb0,0xc0,0},
        };
        bool invalid = false;
        ready = false;
        for (const auto &check : checks) {
            uint8_t value = 0;
            int error = bus.read(check.sink, check.reg, value);
            if (error) return error;
            if ((value & check.mask) != check.value) invalid = true;
            if (!check.sink && check.reg == 0x46) module = value;
        }
        uint8_t lo = 0, hi = 0;
        int error = bus.read(true, 0x51, source);
        // Preserve HIGH (0) or CABC1 (1); CABC2/EXT are not qualified.
        if (!error && (source & 3) > 1) invalid = true;
        if (!error) error = bus.read(true, 0x53, lo);
        if (!error) error = bus.read(true, 0x54, hi);
        if (error) return error;
        if (invalid || (hi & 0xf0) || !(lo | hi)) return configuration;
        level = uint32_t(lo) | uint32_t(hi)<<8;
        ready = on = true;
        return 0;
    }
    template<class Bus> int set(Bus &bus, bool requested) {
        if (!ready || failed) return configuration;
        uint8_t actual = 0, faults = 0, flash = 0, currentSource = 0;
        int error = bus.read(false, 0x46, actual);
        if (!error) error = bus.read(false, 0x08, faults);
        if (!error) error = bus.read(true, 0xb0, flash);
        if (!error) error = bus.read(true, 0x51, currentSource);
        if (!error && (currentSource != source || actual != uint8_t((module & 0x7f) | (on ? 0x80 : 0)) ||
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
    template<class Bus> int brightness(Bus &bus,uint32_t requested) {
        if(!ready || failed || requested<min_level || requested>max_level)return configuration;
        // Read/validate the adopted gate, source, fault and flash state before
        // changing the MOD_A brightness. No CABC/current/voltage reprogramming.
        int error=set(bus,on);
        uint8_t lo=0,hi=0,sync=0;
        if(!error)error=bus.read(true,0x53,lo);
        if(!error)error=bus.read(true,0x54,hi);
        if(!error && (uint32_t(lo)|(uint32_t(hi)<<8))!=level)error=configuration;
        if(!error && level==requested)return 0;
        if(!error)error=bus.read(true,0x65,sync);
        // MOD_B must not be active; retain unrelated upper register bits.
        if(!error && (sync&0x0e))error=configuration;
        if(!error)error=checked_write(bus,true,0x53,uint8_t(requested));
        if(!error)error=checked_write(bus,true,0x54,uint8_t(requested>>8));
        if(!error)error=checked_write(bus,true,0x65,uint8_t(sync&0xf0));
        if(!error)error=checked_write(bus,true,0x65,uint8_t((sync&0xf0)|1));
        if(!error)error=bus.read(true,0x53,lo);
        if(!error)error=bus.read(true,0x54,hi);
        if(!error && (uint32_t(lo)|(uint32_t(hi)<<8))!=requested)error=mismatch;
        if(error)failed=true;else level=requested;
        return error;
    }
};
}
