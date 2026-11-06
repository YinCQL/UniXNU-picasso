// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent SMEM v12/SMP2P wire implementation from Picasso's stock
// smem.c, smp2p.c and lito-smp2p.dtsi. No Linux implementation is copied.
#include "PicassoSmp2p.h"

namespace {
constexpr uint32_t toc = WLAN_SMEM_SIZE - 4096, bytes = 340, padded = 344, need = 16 + padded;
int rd(const PicassoSmemIo *io, uint32_t at, void *out, uint32_t n) {
    return wlan_smread(&io->reader, at, out, n);
}
int wr(const PicassoSmemIo *io, uint32_t at, uint32_t value) {
    if ((at & 3) || at > WLAN_SMEM_SIZE - 4) return -22;
    return io->write32(io->reader.context, at, value);
}
void fence(const PicassoSmemIo *io) { io->barrier(io->reader.context); }
bool locked(const PicassoSmemIo *io) { return io->locked(io->reader.context); }
// Stricter than the survey: validate all allocations and partition overlaps
// before creating a new item, under the remote allocator's hardware mutex.
int partition(const PicassoSmemIo *io, struct wlan_smem_partition *out, PicassoSmemAudit *audit) {
    *audit = {};
    audit->phase = "partition"; audit->index = ~0U;
    int error = wlan_smem_partition(&io->reader, 0, 2, out);
    if (error) return error;
    audit->phase = "toc";
    uint8_t h[32], e[48];
    if (rd(io, toc, h, sizeof h)) return -5;
    const uint32_t count = wlan_sm32(h + 8);
    audit->count = count;
    if (wlan_sm32(h) != 0x434f5424 || wlan_sm32(h + 4) != 1 || count > 64) return -22;
    for (unsigned n = 0; n < count; ++n) {
        audit->phase = "entry-read"; audit->index = n;
        if (rd(io, toc + 32 + 48 * n, e, sizeof e)) return -5;
        uint32_t at = wlan_sm32(e), size = wlan_sm32(e + 4);
        audit->phase = "entry-range";
        audit->offset = at; audit->size = size; audit->flags = wlan_sm32(e + 8);
        audit->hosts = wlan_sm32(e + 12); audit->cacheline = wlan_sm32(e + 16);
        if (!at && !size) {
            if (audit->flags || audit->hosts || audit->cacheline) return -22;
            continue;
        }
        // Picasso V13.0.5.0 XBL, 0x1485a860..0x1485a9f4, records both
        // the TOC page and the legacy header page without $PRT headers.
        // Match their complete defined shape, not just the invalid host ID.
        if (audit->hosts == 0xffffffffU) {
            audit->phase = "reserved";
            unsigned bit = at == toc && size == 4096 && audit->flags == 1 ? 1 :
                           at == 0 && size == 4096 && audit->flags == 3 ? 2 : 0;
            if (!bit || audit->cacheline || (audit->reserved & bit)) return -22;
            audit->reserved |= bit;
            continue;
        }
        if (!at || !size || at < 4096 || (at & 7) || at > toc || size > toc - at) return -22;
        uint16_t a = wlan_sm16(e + 12), b = wlan_sm16(e + 14);
        if ((a == 0 && b == 2) || (a == 2 && b == 0)) continue;
        audit->phase = "overlap";
        if (at < out->offset + out->size && out->offset < at + size) return -22;
    }
    // Stock defaults to 512 when the optional SIII trailer is absent.
    audit->phase = "siii"; audit->index = ~0U;
    if (rd(io, toc + 32 + 48 * count, h, 24)) return -5;
    audit->infoMagic = wlan_sm32(h); audit->infoSize = wlan_sm32(h + 4);
    audit->infoBaseLow = wlan_sm32(h + 8); audit->infoBaseHigh = wlan_sm32(h + 12);
    audit->items = wlan_sm16(h + 16);
    // XBL writes a full 64-bit region base at +8, not a 32-bit address.
    if (audit->infoMagic == 0x49494953 &&
        (audit->infoSize != WLAN_SMEM_SIZE || audit->infoBaseLow != WLAN_SMEM_BASE ||
         audit->infoBaseHigh || audit->items <= 443)) return -22;
    audit->phase = "layout-ok";
    return 0;
}
bool same(const struct wlan_smem_partition &a, const struct wlan_smem_partition &b) {
    return a.offset == b.offset && a.size == b.size && a.uncached == b.uncached &&
        a.cached == b.cached && a.cacheline == b.cacheline;
}
int outValid(const PicassoSmemIo *io, const PicassoSmp2p *s) {
    uint8_t h[40];
    if (!s->created || rd(io, s->out.offset, h, sizeof h)) return -5;
    if (wlan_sm32(h) != 0x504d5324 || wlan_sm32(h + 4) != (1 | (s->features << 8)) ||
        wlan_sm32(h + 8) != (2U << 16) || wlan_sm32(h + 12) != ((1U << 16) | 16) ||
        wlan_sm32(h + 16) != s->flags || wlan_sm32(h + 36) != 0) return -22;
    const char expected[16] = "master-kernel";
    for (unsigned i = 0; i < 16; ++i) if (h[20 + i] != uint8_t(expected[i])) return -22;
    return 0;
}
}

int picasso_smp2p_prepare(const PicassoSmemIo *io, PicassoSmp2p *s) {
    if (!io || !io->reader.read || !io->write32 || !io->barrier || !io->locked || !s) return -22;
    *s = {};
    s->audit.phase = "lock";
    if (!locked(io)) return -16;
    int error = partition(io, &s->partition, &s->audit);
    if (error) return error;
    wlan_smem_item old = {};
    s->audit.phase = "find-out";
    error = wlan_smem_find(&io->reader, &s->partition, 429, &old);
    if (!error) return -17; // An existing item is never ours to zero or adopt.
    if (error != -2) return error;
    const auto &p = s->partition;
    s->audit.phase = "space";
    if (need > p.cached - p.uncached) return -28;
    struct wlan_smem_partition check = {};
    if ((error = partition(io, &check, &s->audit))) return error;
    s->audit.phase = "recheck";
    if (!same(p, check) || !locked(io)) return -11;
    const uint32_t at = p.offset + p.uncached, data = at + 16;
    s->audit.phase = "write-item";
    // Build the whole outgoing item before publishing it to readers. Version
    // is the last SMP2P field; the allocator free pointer is the commit word.
    uint32_t words[need / 4] = {};
    words[0] = 0xa5a5 | (429U << 16); words[1] = padded; words[2] = padded - bytes;
    words[4] = 0x504d5324; words[6] = 2U << 16; words[7] = 16 | (1U << 16);
    const char name[] = "master-kernel";
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) words[9 + i / 4] |= uint32_t(uint8_t(name[i])) << ((i % 4) * 8);
    for (unsigned i = 0; i < need / 4; ++i) if ((error = wr(io, at + 4 * i, words[i]))) return error;
    fence(io);
    // Offer only implemented SSR acknowledgement; data bits remain zero.
    if ((error = wr(io, data + 4, 0x101))) return error;
    fence(io);
    if (!locked(io)) return -16;
    s->audit.phase = "publish";
    if ((error = wr(io, p.offset + 12, p.uncached + need))) return error;
    fence(io);
    s->out = {data, bytes}; s->features = 1; s->created = true;
    // Verify through the independent read path. Never roll back a published
    // free pointer: a peer may already have appended its own allocation.
    if ((error = partition(io, &check, &s->audit))) return s->failed = true, error;
    s->audit.phase = "verify-published";
    if (check.offset != p.offset || check.size != p.size || check.uncached < p.uncached + need) return s->failed = true, -11;
    if ((error = wlan_smem_find(&io->reader, &check, 429, &old))) return s->failed = true, error;
    if (old.offset != data || old.size != bytes) return s->failed = true, -22;
    error = outValid(io, s);
    if (error) s->failed = true;
    else s->audit.phase = "prepared";
    return error;
}

int picasso_smp2p_service(const PicassoSmemIo *io, PicassoSmp2p *s, PicassoSmp2pEvent *event) {
    if (!io || !io->reader.read || !io->write32 || !io->barrier || !s || !event || !s->created || s->failed) return -22;
    *event = {};
    struct wlan_smem_partition p = {};
    int error = partition(io, &p, &s->audit);
    if (error) return error;
    s->audit.phase = "peer";
    if (p.offset != s->partition.offset || p.size != s->partition.size ||
        p.uncached < s->partition.uncached + need || p.cacheline != s->partition.cacheline) return -22;
    wlan_smem_item out = {}, in = {};
    if ((error = wlan_smem_find(&io->reader, &p, 429, &out))) return error;
    if (out.offset != s->out.offset || out.size != bytes || (error = outValid(io, s))) return error ? error : -22;
    if ((error = wlan_smem_find(&io->reader, &p, 443, &in))) return error;
    if (in.size != bytes) return -22;
    uint8_t h[20], after[20];
    if (rd(io, in.offset, h, sizeof h)) return -5;
    // A peer publishes version last. Zero version is still initializing.
    if (!h[4]) return -11;
    if (wlan_sm32(h) != 0x504d5324 || h[4] != 1 || wlan_sm32(h + 8) != 2 ||
        wlan_sm16(h + 12) > 16 || wlan_sm16(h + 14) > wlan_sm16(h + 12)) return -22;
    uint32_t value = 0;
    error = wlan_smp2p_entry(&io->reader, &in, 2, 0, "slave-kernel", &value);
    if (error && error != -2) return error;
    event->hasValue = !error;
    if (rd(io, in.offset, after, sizeof after)) return -5;
    for (unsigned i = 0; i < sizeof h; ++i) if (h[i] != after[i]) return -11;
    const uint32_t features = (wlan_sm32(h + 4) >> 8) & 1;
    event->value = value; event->changed = event->hasValue ? value ^ s->last : 0;
    event->features = features; event->flags = wlan_sm32(h + 16);
    // A peer changing the negotiated features is a new session, not ready.
    if (s->negotiated && s->features != features) return -57;
    if (!s->negotiated && s->features != features) {
        if ((error = wr(io, s->out.offset + 4, 1 | (features << 8)))) return s->failed = true, error;
        fence(io); event->kick = true;
    }
    s->features = features; s->negotiated = true;
    const bool restart = features && ((event->flags & 1) != ((s->flags >> 1) & 1));
    if (restart) {
        // Caller receives restart and must invalidate readiness before PAS.
        s->flags = (s->flags & ~2U) | ((event->flags & 1) << 1);
        if ((error = wr(io, s->out.offset + 16, s->flags))) return s->failed = true, error;
        fence(io); event->restart = event->kick = true;
    }
    // Negotiate/acknowledge even before the remote publishes slave-kernel.
    if (event->hasValue) s->last = value;
    return 0;
}
