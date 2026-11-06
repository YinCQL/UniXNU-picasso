// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared APPS DRV2 transaction engine. Callers serialize the physical RSC.
#ifndef PICASSO_RPMH_H
#define PICASSO_RPMH_H
#include <stdint.h>
struct PicassoRscIo {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*delay)(void *, unsigned);
};
struct PicassoRscResult { uint32_t irq, status, command, control, enabled; unsigned spins; bool poisoned; };
// Picasso build179 measured version 2.5. No guessed fallback to other layouts.
static inline int picasso_rsc_send(const PicassoRscIo *io, unsigned tcs, uint32_t address,
                                 uint32_t value, PicassoRscResult *r) {
    if (!r) return -22;
    *r = {};
    if (!io || !io->read || !io->write || !io->barrier || !io->delay) return -22;
    if (tcs >= 2 || (address & 3) || !address || ((io->read(io->context, 0) >> 16) & 0xff) != 2) return -22;
    const uint32_t at = 0xd00 + 672 * tcs, bit = 1U << tcs;
    // Never reset a busy controller or consume a prior owner's completion.
    r->irq = io->read(io->context, 0xd04);
    r->status = io->read(io->context, at + 0x18);
    r->control = io->read(io->context, at + 0x14);
    r->enabled = io->read(io->context, at + 0x1c);
    // This owner serializes both active TCSs. Also reject an unknown request
    // on the other TCS: two concurrent requests for one address are invalid.
    const uint32_t other = 0xd00 + 672 * (tcs ^ 1);
    if (!(r->status & 1) || r->enabled || (r->irq & 3) ||
        !(io->read(io->context, other + 0x18) & 1) || io->read(io->context, other + 0x1c)) return -16;
    const uint32_t irqEnable = io->read(io->context, 0xd00);
    io->write(io->context, 0xd00, irqEnable & ~bit);
    auto sync = [&](uint32_t reg, uint32_t data) {
        io->write(io->context, reg, data); io->barrier(io->context);
        for (unsigned n = 0; n < 100; ++n) {
            if (io->read(io->context, reg) == data) return true;
            io->delay(io->context, 1);
        }
        return false;
    };
    // An idle, disabled boot TCS may retain its mode bits. Reset in the stock
    // hardware-required order before enabling any new command.
    uint32_t control = r->control & ~(1U << 24);
    if (!sync(at + 0x14, control) || !sync(at + 0x14, control & ~(1U << 16))) {
        r->poisoned = true; return -60;
    }
    // Confirm the new payload before setting enable/trigger. A dropped write
    // must not execute a previous command and accept its completion.
    bool issued = sync(at + 0x10, 0) && // No stale WAIT_FOR_CMPL mask.
        sync(at + 0x30, 8 | (1U << 8) | (1U << 16)) &&
        sync(at + 0x34, address) && sync(at + 0x38, value) &&
        sync(at + 0x1c, 1) && sync(at + 0x14, 1U << 16) && sync(at + 0x14, (1U << 16) | (1U << 24));
    if (issued) for (; r->spins < 10000; ++r->spins) {
        r->irq = io->read(io->context, 0xd04);
        r->status = io->read(io->context, at + 0x18);
        r->command = io->read(io->context, at + 0x3c);
        // Command status must include ISSUED and COMPLETED, with TCS idle.
        // The per-command status resets on a fresh trigger; idle was required
        // before issuing. The disabled IRQ may not latch (measured on Picasso).
        if ((r->status & 1) && (r->command & 0x10100) == 0x10100) break;
        io->delay(io->context, 10);
    }
    if (!issued || r->spins == 10000) {
        r->poisoned = true;
        // Hardware may still own the command. Keep its IRQ masked and do not
        // clear enable, trigger or status. All later clients must refuse reuse.
        return -60;
    }
    if (!sync(at + 0x14, 1U << 16) || !sync(at + 0x14, 0)) {
        r->poisoned = true; return -60;
    }
    if (!sync(at + 0x1c, 0)) { r->poisoned = true; return -60; }
    io->write(io->context, 0xd08, bit);
    io->barrier(io->context);
    // Restore only our bit, preserving any unrelated controller changes.
    io->write(io->context, 0xd00, (io->read(io->context, 0xd00) & ~bit) | (irqEnable & bit));
    io->barrier(io->context);
    return 0;
}

struct PicassoArcResource { uint32_t address, index, level; };
static inline uint16_t picasso_db16(const uint8_t *p) { return p[0] | (uint16_t(p[1]) << 8); }
static inline uint32_t picasso_db32(const uint8_t *p) { return picasso_db16(p) | (uint32_t(picasso_db16(p + 2)) << 16); }
static inline int picasso_arc_lookup(const uint8_t *db, uint32_t size, const char *name,
                                    uint32_t minimum, PicassoArcResource *out) {
    if (!db || !name || !out || size < 144 || picasso_db32(db) != 1 || picasso_db32(db + 4) != 0x0c0330db) return -22;
    uint8_t query[8] = {}; unsigned length = 0;
    while (length < 8 && name[length]) {query[length] = uint8_t(name[length]); ++length;}
    if (!length || (length == 8 && name[length])) return -22;
    unsigned found = 0;
    PicassoArcResource result = {};
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *slave = db + 8 + i * 16;
        if (!picasso_db16(slave)) continue;
        uint32_t headers = 144 + picasso_db16(slave + 2), aux = 144 + picasso_db16(slave + 4);
        unsigned count = picasso_db16(slave + 6);
        if (headers > size || count > (size - headers) / 24 || aux > size) return -22;
        for (unsigned j = 0; j < count; ++j) {
            const uint8_t *e = db + headers + j * 24;
            bool match = true;
            for (unsigned k = 0; k < 8; ++k) if (query[k] != e[k]) match = false;
            if (!match) continue;
            if (++found > 1 || picasso_db16(slave) != 3) return -22; // ARC accelerator
            uint32_t address = picasso_db32(e + 16), at = aux + picasso_db16(e + 22);
            unsigned n = picasso_db16(e + 20);
            if ((address & 3) || ((address >> 16) & 7) != 3 || at > size || n > size - at) return -22;
            if (minimum == UINT32_MAX) { result = {address, 0, 0}; continue; } // XO needs address only.
            if (!n || n > 32 || (n & 1)) return -22;
            uint32_t prior = 0; bool selected = false, padding = false;
            for (unsigned k = 0; k < n / 2; ++k) {
                uint32_t level = picasso_db16(db + at + 2 * k);
                if (!k && level) return -22;
                if (k && !level) padding = true;
                if (level && (padding || level < prior)) return -22;
                if (!padding && !selected && level >= minimum) {result = {address, k, level}; selected = true;}
                prior = level;
            }
            if (!selected) return -34;
        }
    }
    if (!found) return -2;
    *out = result; return 0;
}
// Defined alongside the existing GPU RSC owner so GPU and future PAS callers
// share the same gate and permanent timeout quarantine.
extern "C" int picasso_rpmh_adsp_plan(void);
extern "C" int picasso_rpmh_active_write(uint32_t address, uint32_t value);
#endif
