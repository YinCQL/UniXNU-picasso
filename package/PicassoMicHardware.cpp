// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PicassoMicHardware.h"

namespace {
constexpr uint64_t CodecId = 0x000d01170223ULL;
constexpr uint32_t BusVersion = 0x01050001;
constexpr uint32_t BusErrors = 0x133f8;
constexpr uint32_t FrameMask = 0xff07ff;
constexpr uint32_t CaptureFrame = (5U << 16) | (1U << 3) | 7U;
constexpr unsigned Swr = PicassoMicSwr, Tx = PicassoMicTx, Slave = PicassoMicSlave;

void stage(PicassoMicHardwareCore &h, unsigned value) {
    h.status.stage = value;
    h.status.dirty = h.savedCount;
    ++h.status.events;
    if (h.io.note) h.io.note(h.io.context, &h.status);
}
int rd(PicassoMicHardwareCore &h, unsigned region, uint32_t reg, uint32_t &value) {
    h.status.lastRegion = region; h.status.lastRegister = reg;
    int e = h.io.read(h.io.context, region, reg, &value);
    if (!e) h.status.lastValue = value;
    return e;
}
int wr(PicassoMicHardwareCore &h, unsigned region, uint32_t reg, uint32_t value) {
    h.status.lastRegion = region; h.status.lastRegister = reg; h.status.lastValue = value;
    return h.io.write(h.io.context, region, reg, value);
}
void delay(PicassoMicHardwareCore &h, unsigned us) { h.io.delay(h.io.context, us); }
int busError(PicassoMicHardwareCore &h) {
    uint32_t v = 0; int e = rd(h, Swr, 0x200, v);
    return e ? e : ((v & BusErrors) ? -5 : 0);
}
int fifoSpace(PicassoMicHardwareCore &h) {
    if (!h.writeDepth) return -19;
    for (unsigned i = 0; i < 20; ++i) {
        uint32_t v = 0; int e = busError(h);
        if (!e) e = rd(h, Swr, 0x30c, v);
        if (e) return e;
        if (((v >> 8) & 31) < h.writeDepth) return 0;
        delay(h, 500);
    }
    return -110;
}
unsigned nextCommand(PicassoMicHardwareCore &h) {
    h.commandId = (h.commandId + 1) % 15;
    return h.commandId;
}
int slaveRead(PicassoMicHardwareCore &h, uint32_t reg, uint32_t &value) {
    h.status.lastSlaveRegister = reg; h.status.lastSlaveCommand = 1;
    if (!h.status.slave || reg > 0xffff) return -19;
    uint32_t v = 0; int e = fifoSpace(h);
    if (!e) e = rd(h, Swr, 0x30c, v);
    // Never mistake an old response for this transaction, including ID wrap.
    if (!e && (v & 0x1f0000)) e = -71;
    if (e) return e;
    unsigned id = nextCommand(h);
    e = wr(h, Swr, 0x304, (1U << 24) | (h.status.slave << 20) | (id << 16) | reg);
    if (e) return e;
    delay(h, 250);
    for (unsigned i = 0; i < 20; ++i) {
        e = busError(h);
        if (!e) e = rd(h, Swr, 0x30c, v);
        if (e) return e;
        if (v & 0x1f0000) {
            e = rd(h, Swr, 0x318, v);
            if (e) return e;
            if (((v >> 8) & 15) != id) return -71;
            value = v & 255;
            return 0;
        }
        delay(h, 500);
    }
    return -110;
}
int slaveWrite(PicassoMicHardwareCore &h, uint32_t reg, uint32_t value) {
    h.status.lastSlaveRegister = reg; h.status.lastSlaveCommand = 2;
    if (!h.status.slave || reg > 0xffff || value > 255) return -22;
    int e = fifoSpace(h);
    if (!e) e = wr(h, Swr, 0x300, (value << 24) | (h.status.slave << 20) |
                  (nextCommand(h) << 16) | reg);
    if (!e) { delay(h, 250); e = busError(h); }
    return e;
}
int readReg(PicassoMicHardwareCore &h, unsigned region, uint32_t reg, uint32_t &value) {
    return region == Slave ? slaveRead(h, reg, value) : rd(h, region, reg, value);
}
int writeReg(PicassoMicHardwareCore &h, unsigned region, uint32_t reg, uint32_t value) {
    return region == Slave ? slaveWrite(h, reg, value) : wr(h, region, reg, value);
}
int update(PicassoMicHardwareCore &h, unsigned region, uint32_t reg,
           uint32_t mask, uint32_t value, bool save = true) {
    uint32_t old = 0, got = 0; int e = readReg(h, region, reg, old);
    if (e) return e;
    value = (old & ~mask) | (value & mask);
    if (value == old) return 0;
    if (save) {
        if (h.savedCount == sizeof(h.saved) / sizeof(h.saved[0])) return -28;
        // Journal before a write: a transport error can follow publication.
        h.saved[h.savedCount++] = {region, reg, mask, old};
        h.status.dirty = h.savedCount;
    }
    e = writeReg(h, region, reg, value);
    if (!e) e = readReg(h, region, reg, got);
    if (!e && (got & mask) != (value & mask)) e = -5;
    return e;
}
struct Setting { unsigned region; uint32_t reg, mask, value; };
int settings(PicassoMicHardwareCore &h, const Setting *s, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        int e = update(h, s[i].region, s[i].reg, s[i].mask, s[i].value);
        if (e) return e;
    }
    return 0;
}
template<unsigned N> int settings(PicassoMicHardwareCore &h, const Setting (&s)[N]) {
    return settings(h, s, N);
}
int switchBank(PicassoMicHardwareCore &h, unsigned bank) {
    uint32_t frame = 0; int e = rd(h, Swr, 0x101c + 0x40 * bank, frame);
    if (!e) e = wr(h, Swr, 0x208, 0x400);
    if (!e) e = fifoSpace(h);
    if (!e) e = wr(h, Swr, 0x300, ((frame & 255) << 24) | (15U << 20) |
                  (15U << 16) | (0x60 + bank * 0x10));
    if (e) return e;
    // SWR 1.5.1 needs at least 10 ms for a broadcast. The observed completion
    // and active-bank bit are both required, never inferred from the delay.
    delay(h, 10000);
    for (unsigned i = 0; i < 40; ++i) {
        uint32_t irq = 0, active = 0;
        e = busError(h);
        if (!e) e = rd(h, Swr, 0x200, irq);
        if (!e) e = rd(h, Swr, 0x104c, active);
        if (e) return e;
        if ((irq & 0x400) && (active & 1) == bank) {
            h.status.activeBank = bank;
            return wr(h, Swr, 0x208, 0x400);
        }
        delay(h, 500);
    }
    return -110;
}
int pins(PicassoMicHardwareCore &h) {
    const unsigned gpio[4] = {0, 1, 2, 14};
    // All four pins are the board's TX group. Reject an unrelated mux before
    // writing any pin. GPIO14 shares slew bit 0 in the exported lito DT.
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t v = 0; int e = rd(h, PicassoMicLpi, gpio[i] * 0x1000, v);
        if (e) return e;
        if (((v >> 2) & 15) > 1) return -16;
    }
    for (unsigned i = 0; i < 4; ++i) {
        int e = update(h, PicassoMicLpi, gpio[i] * 0x1000, 0x3ff, 4 | (i ? 2 : 0));
        if (e) return e;
    }
    int e = update(h, PicassoMicSlew, 0, 0x3f, 0x15);
    uint32_t ctl = 0, latch = 0;
    if (!e) e = rd(h, PicassoMicReset, 0, ctl);
    if (!e) e = rd(h, PicassoMicReset, 4, latch);
    if (e) return e;
    if ((ctl >> 2) & 15) return -16;
    // An already released codec stays released; do not pulse a shared reset.
    if ((ctl & 0x200) && (latch & 2)) return 0;
    e = update(h, PicassoMicReset, 4, 2, 2);
    if (!e) e = update(h, PicassoMicReset, 0, 0x3ff, 0x200 | (7U << 6));
    if (!e) delay(h, 20);
    return e;
}
int clocks(PicassoMicHardwareCore &h) {
    const Setting s[] = {
        {PicassoMicVa, 0, 1, 1}, {PicassoMicVa, 4, 1, 1}, {PicassoMicVa, 0x80, 2, 2},
        {Tx, 0x90, 1, 1}, {Tx, 0, 1, 1}, {Tx, 4, 1, 1}
    };
    int e = settings(h, s);
    uint32_t gate = 0;
    if (!e) e = rd(h, Tx, 8, gate);
    // This diagnostic owns a previously stopped TX bus only.
    if (!e && (gate & 2)) e = -16;
    // Enabling the gate permits identity/ownership checks. A reset would
    // destroy inherited master state before it could be inspected or saved.
    if (!e) e = update(h, Tx, 8, 1, 1);
    if (!e) delay(h, 20);
    return e;
}
int initBus(PicassoMicHardwareCore &h) {
    uint32_t params = 0, cfg = 0;
    int e = rd(h, Swr, 0, h.status.swrVersion);
    if (!e && h.status.swrVersion != BusVersion) e = -19;
    if (!e) e = rd(h, Swr, 0x100, params);
    if (!e) e = rd(h, Swr, 4, cfg);
    if (e) return e;
    h.writeDepth = (params >> 10) & 31;
    if (!h.writeDepth || !((params >> 15) & 31)) return -19;
    if (cfg & 1) return -16;
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned port = 0; port < 5; ++port) {
            uint32_t v = 0; e = rd(h, Swr, 0x1124 + port * 0x100 + bank * 0x40, v);
            if (e) return e;
            if (v & 0xff000000) return -16;
        }
    const Setting s[] = {
        {Swr, 0x101c, FrameMask, (47U << 16) | 8},
        {Swr, 0x500, 1, 1}, {Swr, 0x1048, 0x3e0000, 0x3e0000},
        {Swr, 0x314, 0x80000007, 0x80000003}
    };
    e = settings(h, s);
    // CLK_START is a command, not a persistent setting to read back or replay
    // during restoration. Enumeration below verifies the resulting bus state.
    if (!e) e = wr(h, Swr, 0x1044, 2);
    const Setting irq[] = {
        {Swr, 4, 3, 2}, {Swr, 0x204, 0x1ffff, 0x1fdfd},
        {Swr, 0x210, 0x1ffff, 0}
    };
    if (!e) e = settings(h, irq);
    if (!e) e = wr(h, Swr, 0x208, 0x1ffff);
    if (!e) e = update(h, Swr, 4, 3, 3);
    return e;
}
int enumerate(PicassoMicHardwareCore &h) {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        uint32_t states = 0; int e = busError(h);
        if (!e) e = rd(h, Swr, 0x1090, states);
        if (e) return e;
        for (unsigned slave = 1; slave < 12; ++slave) {
            unsigned state = (states >> (2 * slave)) & 3;
            if (state != 1 && state != 2) continue;
            uint32_t lo = 0, hi = 0;
            e = rd(h, Swr, 0x530 + 8 * slave, lo);
            if (!e) e = rd(h, Swr, 0x534 + 8 * slave, hi);
            if (e) return e;
            uint64_t id = (uint64_t(hi & 0xffff) << 32) | lo;
            if (id != CodecId) continue;
            h.status.slave = slave; h.status.slaveId = id;
            uint32_t chip = 0;
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t v = 0; e = slaveRead(h, 0x3401 + i, v);
                if (e) return e;
                chip |= v << (i * 8);
            }
            h.status.chipId = chip;
            return (!chip || chip == 0xffffffff) ? -19 : 0;
        }
        delay(h, 1000);
    }
    return -19;
}
int codec(PicassoMicHardwareCore &h) {
    // TX/common initialization only; headphone/receiver controls stay intact.
    int e = update(h, Slave, 0x3103, 0x8e, 0x8e);
    if (!e) { delay(h, 1000); e = update(h, Slave, 0x3103, 0x40, 0x40); }
    if (!e) delay(h, 1000);
    const Setting bias[] = {
        {Slave, 0x312a, 0x10, 0}, {Slave, 0x3029, 0xf0, 0x80},
        {Slave, 0x3001, 0xc0, 0xc0}
    };
    if (!e) e = settings(h, bias);
    if (!e) { delay(h, 10000); e = update(h, Slave, 0x3001, 0x40, 0); }
    const Setting init[] = {
        {Slave, 0x345b, 2, 2}, {Slave, 0x31e1, 0xff, 0x14}, {Slave, 0x31df, 0x1f, 8},
        {Slave, 0x34d0, 0xff, 0x55}, {Slave, 0x34d1, 0xff, 0x44},
        {Slave, 0x34d2, 0xff, 0x11}, {Slave, 0x34d3, 0xff, 0}, {Slave, 0x34d4, 0xff, 0},
        {Slave, 0x306b, 0xe0, 0xe0}, {Slave, 0x306e, 0xe0, 0xe0},
        {Slave, 0x3071, 0xe0, 0xe0}, {Slave, 0x3074, 0xe0, 0xe0}, {Slave, 0x308f, 1, 0}
    };
    if (!e) e = settings(h, init);
    uint32_t fuse = 0;
    if (!e) e = slaveRead(h, 0x34ce, fuse);
    if (!e) e = update(h, Slave, 0x3103, 0x0e, (fuse & 7) << 1);
    const Setting adc[] = {
        {Slave, 0x3405, 0xff, 0}, {Slave, 0x3022, 0x3f, 35}, // 2750 mV
        {Slave, 0x3409, 0xf0, 0xf0}, {Slave, 0x3408, 0x10, 0x10},
        {Slave, 0x3417, 1, 1}, {Slave, 0x306c, 1, 1}, {Slave, 0x306f, 1, 1},
        {Slave, 0x3072, 1, 1}, {Slave, 0x3075, 1, 1}, {Slave, 0x3022, 0xc0, 0x40}
    };
    if (!e) e = settings(h, adc);
    if (!e) delay(h, 2000);
    const Setting enable[] = {
        {Slave, 0x300e, 0x1f, 12}, {Slave, 0x3408, 0x18, 0x18},
        {Slave, 0x300f, 0x40, 0x40}, {Slave, 0x3410, 0x0f, 3},
        {Slave, 0x3409, 0x10, 0x10}, {Slave, 0x300f, 0x40, 0}, {Slave, 0x3457, 3, 2}
    };
    if (!e) e = settings(h, enable);
    return e;
}
int port(PicassoMicHardwareCore &h, unsigned bank) {
    unsigned m = bank * 0x40, s = bank * 0x10;
    const Setting config[] = {
        // TX lacks the RX-only disable-div2-clk-switch property. Apply the
        // host-divider control to the identified codec, in both banks.
        {Slave, 0xe0 + s, 1, 1},
        {Slave, 0x122 + s, 0xff, 1}, {Slave, 0x124 + s, 0xff, 0},
        {Slave, 0x125 + s, 0xff, 0}, {Slave, 0x128 + s, 7, 1},
        {Swr, 0x1228 + m, 7, 1}, {Swr, 0x1234 + m, 0xff, 0xf0},
        {Swr, 0x1224 + m, 0xffffffff, 0x01000001}, {Slave, 0x120 + s, 0xff, 1},
        {Swr, 0x101c + m, FrameMask, CaptureFrame}
    };
    h.portsTouched = true;
    return settings(h, config);
}
int route(PicassoMicHardwareCore &h) {
    uint32_t existing = 0; int e = rd(h, Tx, 0x400, existing);
    if (!e && (existing & 0x20)) e = -16;
    if (e) return e;
    const Setting s[] = {
        {Tx, 0x100, 0x0f, 1}, {Tx, 0x104, 3, 1},
        {Tx, 0x404, 0xe7, 0x40}, // analog, normal mode, 150 Hz high-pass, BCS off
        {Tx, 0x42c, 0x3f, 0x0a}, {Tx, 0x400, 0x3f, 0x34},
        {Tx, 0x40c, 0xff, 0} // unity digital gain for initial acoustic qualification
    };
    e = settings(h, s);
    uint32_t bank = 0;
    if (!e) e = rd(h, Swr, 0x104c, bank);
    if (e) return e;
    bank = (bank & 1) ^ 1;
    e = port(h, bank);
    if (!e) e = switchBank(h, bank);
    if (!e) e = port(h, bank ^ 1);
    if (!e) e = update(h, Tx, 0x418, 3, 2);
    if (!e) e = update(h, Tx, 0x418, 3, 1);
    if (!e) delay(h, 6000);
    return e;
}
int stop(PicassoMicHardwareCore &h) {
    h.status.ready = 0; h.status.unmuted = 0; h.status.restoreError = 0;
    stage(h, PicassoMicRestoring);
    // Once data ports have been touched, commit disabled channels before
    // restoring analog settings. No later step can drop the bus clocks early.
    int e = 0;
    if (h.portsTouched) {
        e = update(h, Tx, 0x400, 0x10, 0x10, false);
        for (unsigned bank = 0; !e && bank < 2; ++bank) {
            e = update(h, Slave, 0x120 + bank * 0x10, 0xff, 0, false);
            if (!e) e = update(h, Swr, 0x1224 + bank * 0x40, 0xff000000, 0, false);
        }
        uint32_t bank = 0;
        if (!e) e = rd(h, Swr, 0x104c, bank);
        if (!e) e = switchBank(h, (bank & 1) ^ 1);
        if (!e) h.portsTouched = false;
    }
    // Restore strictly in reverse order. On failure retain this and all older
    // entries; release of parent clocks would destroy the ability to retry.
    while (!e && h.savedCount) {
        const auto &s = h.saved[h.savedCount - 1];
        e = update(h, s.region, s.reg, s.mask, s.value, false);
        if (!e) --h.savedCount;
    }
    if (!e && h.status.rails) {
        e = h.io.rails(h.io.context, 2);
        if (!e) h.status.rails = 0;
    }
    h.status.dirty = h.savedCount;
    h.status.restoreError = e;
    stage(h, e ? PicassoMicRestoring : PicassoMicStopped);
    return e;
}
int prepare(PicassoMicHardwareCore &h) {
    if (h.status.ready) return 0;
    if (h.savedCount || h.status.rails) return -16;
    h.status = {}; h.status.version = 1; h.commandId = 0; h.writeDepth = 0;
    h.portsTouched = false;
    stage(h, PicassoMicRails);
    int e = h.io.rails(h.io.context, 0);
    if (!e) h.status.rails = 1;
    if (!e) { stage(h, PicassoMicPins); e = pins(h); }
    if (!e) { stage(h, PicassoMicClocks); e = clocks(h); }
    if (!e) { stage(h, PicassoMicBus); e = initBus(h); }
    if (!e) { stage(h, PicassoMicEnumerate); e = enumerate(h); }
    if (!e) { stage(h, PicassoMicCodec); e = codec(h); }
    if (!e) { stage(h, PicassoMicRoute); e = route(h); }
    if (!e) e = h.io.rails(h.io.context, 1);
    if (!e) { h.status.ready = 1; stage(h, PicassoMicPrepared); }
    if (e) { h.status.error = e; h.status.failedStage = h.status.stage; }
    // The caller orders rollback after DSP teardown; never tear hardware down
    // here because a failed AFE operation may still own the capture DMA pool.
    return e;
}
int unmute(PicassoMicHardwareCore &h) {
    if (!h.status.ready) return -19;
    if (h.status.unmuted) return 0;
    // Analog TX settling is part of worker work, never a receive callback.
    delay(h, 100000);
    int e = h.io.rails(h.io.context, 1);
    if (!e) e = update(h, Slave, 0x300f, 0x40, 0);
    if (!e) e = update(h, Tx, 0x418, 3, 2);
    if (!e) { delay(h, 20); e = update(h, Tx, 0x418, 3, 1); }
    if (!e) e = update(h, Tx, 0x400, 0x10, 0);
    if (!e) { h.status.unmuted = 1; stage(h, PicassoMicRunning); }
    if (e) { h.status.error = e; h.status.failedStage = h.status.stage; }
    return e;
}
int check(PicassoMicHardwareCore &h) {
    if (!h.status.ready || !h.status.unmuted || !h.status.slave) return -19;
    int e = h.io.rails(h.io.context, 1);
    if (!e) e = busError(h);
    uint32_t slaves = 0;
    if (!e) e = rd(h, Swr, 0x1090, slaves);
    unsigned state = (slaves >> (h.status.slave * 2)) & 3;
    if (!e && state != 1 && state != 2) e = -19;
    if (e) {
        h.status.error = e; h.status.failedStage = h.status.stage;
        h.status.ready = 0;
    }
    return e;
}
} // namespace

int picasso_mic_hardware_action(PicassoMicHardwareCore &h, unsigned action) {
    if (!h.io.read || !h.io.write || !h.io.delay || !h.io.rails) return -22;
    if (action == PicassoMicPrepare) return prepare(h);
    if (action == PicassoMicUnmute) return unmute(h);
    if (action == PicassoMicStop) return stop(h);
    if (action == PicassoMicCheck) return check(h);
    return -22;
}

#ifndef PICASSO_MIC_HARDWARE_HOST
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
extern "C" int picasso_mic_rails_acquire();
extern "C" int picasso_mic_rails_check();
extern "C" int picasso_mic_rails_release();
namespace {
struct Runtime {
    IOMemoryMap *map[PicassoMicRegionCount];
    uintptr_t va[PicassoMicRegionCount];
    PicassoMicHardwareCore core;
};
Runtime runtime;
IOLock *hardwareLock;
constexpr uint64_t Address[] = {0x03220000, 0x03230000, 0x03370000,
                               0x033c0000, 0x0355a000, 0x0f539000};
constexpr unsigned Length[] = {0x1000, 0x2000, 0x1000, 0xf000, 0x1000, 0x1000};
void barrier() {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
int runtimeRead(void *ctx, unsigned region, uint32_t reg, uint32_t *value) {
    auto &r = *static_cast<Runtime *>(ctx);
    if (!value || region >= PicassoMicRegionCount || (reg & 3) ||
        reg >= Length[region] || !r.va[region]) return -22;
    *value = *reinterpret_cast<volatile uint32_t *>(r.va[region] + reg);
    barrier(); return 0;
}
int runtimeWrite(void *ctx, unsigned region, uint32_t reg, uint32_t value) {
    auto &r = *static_cast<Runtime *>(ctx);
    if (region >= PicassoMicRegionCount || (reg & 3) ||
        reg >= Length[region] || !r.va[region]) return -22;
    *reinterpret_cast<volatile uint32_t *>(r.va[region] + reg) = value;
    barrier(); return 0;
}
void runtimeDelay(void *, unsigned us) {
    if (us >= 1000) IOSleep((us + 999) / 1000); else IODelay(us);
}
int runtimeRails(void *, unsigned action) {
    return action == 0 ? picasso_mic_rails_acquire() :
           action == 1 ? picasso_mic_rails_check() : picasso_mic_rails_release();
}
void runtimeNote(void *, const PicassoMicHardwareStatus *s) {
    IOLog("PICASSO_MIC hardware stage=%u error=%d restore_error=%d dirty=%u slave=%u id=%llx ready=%u acoustic_verified=0\n",
          s->stage, s->error, s->restoreError, s->dirty, s->slave, s->slaveId, s->ready);
}
int mapRuntime(Runtime &r) {
    for (unsigned i = 0; i < PicassoMicRegionCount; ++i) {
        if (r.map[i]) continue;
        auto *mem = IODeviceMemory::withRange(Address[i], Length[i]);
        if (!mem) return -12;
        r.map[i] = mem->map(kIOMapAnywhere | kIOMapInhibitCache); mem->release();
        if (!r.map[i]) return -12;
        r.va[i] = uintptr_t(r.map[i]->getVirtualAddress());
        if (!r.va[i]) return -12;
    }
    r.core.io = {&r, runtimeRead, runtimeWrite, runtimeDelay, runtimeRails, runtimeNote};
    return 0;
}
} // namespace
extern "C" int picasso_mic_hardware(unsigned action) {
    // Actions come from the single microphone service worker. The lock also
    // protects snapshots; it is created before the first state publication.
    if (!hardwareLock) hardwareLock = IOLockAlloc();
    if (!hardwareLock) return -12;
    IOLockLock(hardwareLock);
    int e = mapRuntime(runtime);
    if (!e) e = picasso_mic_hardware_action(runtime.core, action);
    if (e) {
        const auto &s = runtime.core.status;
        IOLog("PICASSO_MIC hardware-failed action=%u result=%d stage=%u failed_stage=%u region=%u reg=%x value=%x slave_reg=%x slave_op=%u dirty=%u\n",
              action, e, s.stage, s.failedStage, s.lastRegion, s.lastRegister, s.lastValue,
              s.lastSlaveRegister, s.lastSlaveCommand, s.dirty);
    }
    IOLockUnlock(hardwareLock); return e;
}
extern "C" void picasso_mic_hardware_snapshot(PicassoMicHardwareStatus *out) {
    if (!out) return;
    if (!hardwareLock) { *out = {}; out->version = 1; return; }
    IOLockLock(hardwareLock); *out = runtime.core.status; IOLockUnlock(hardwareLock);
}
#endif
