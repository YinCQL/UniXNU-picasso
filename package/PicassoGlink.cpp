// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire facts: Picasso stock qcom_glink_{smem,native}.c. Independent implementation.
#include "PicassoGlink.h"
#include "PicassoApr.h"
namespace {
int fail(PicassoGlink *s, int error) {
    s->failed = true; s->error = error; s->ready = false;
    s->apr.ready = false; s->apr.pending = false;
    if (!s->apr.error) s->apr.error = error;
    picasso_qrtr_invalidate(&s->qrtr, error);
    for (auto &c : s->channel) c.up = false;
    return error;
}
int rd(const PicassoSmemIo *io, uint32_t at, uint32_t *v) {
    uint8_t bytes[4]; int error = wlan_smread(&io->reader, at, bytes, 4);
    if (!error) *v = wlan_sm32(bytes);
    return error;
}
int wr(const PicassoSmemIo *io, PicassoGlink *s, uint32_t at, uint32_t v) {
    int error = io->write32(io->reader.context, at, v);
    return error ? fail(s, error) : 0;
}
void fence(const PicassoSmemIo *io) { io->barrier(io->reader.context); }
int transmit(const PicassoSmemIo *io, PicassoGlink *s, const uint32_t *words, uint32_t bytes, bool *kick) {
    uint32_t tail = 0, head = 0;
    if (rd(io, s->desc.offset, &tail) || rd(io, s->desc.offset + 4, &head)) return fail(s, -5);
    if (head != s->txHead || tail >= s->tx.size || (tail & 7)) return fail(s, -22);
    uint32_t free = tail > head ? tail - head : s->tx.size - head + tail;
    if (!bytes || (bytes & 7) || bytes > s->tx.size - 16) return fail(s, -22);
    bool notify = free < bytes + 16;
    // Use the reserved READ_NOTIF slot once. The remote rings us after draining
    // our TX FIFO. The worker itself never waits for that notification.
    const uint32_t notification[2] = {13, 0};
    if (notify) {
        if (s->readNotify || free < 16) return -11;
        words = notification; bytes = 8;
    }
    fence(io); // acquire peer's released slots before reusing their payload
    int error;
    for (uint32_t i = 0; i < bytes; i += 4)
        if ((error = wr(io, s, s->tx.offset + (head + i) % s->tx.size, words[i / 4]))) return error;
    fence(io); // publish payload before APPS TX head
    uint32_t next = (head + bytes) % s->tx.size;
    if ((error = wr(io, s, s->desc.offset + 4, next))) return error;
    fence(io); s->txHead = next; *kick = true;
    s->readNotify = notify;
    return notify ? -11 : 0;
}
int send(const PicassoSmemIo *io, PicassoGlink *s, unsigned command, bool *kick) {
    const uint32_t words[2] = {command | (1U << 16), 0};
    return transmit(io, s, words, sizeof(words), kick);
}
int peek(const PicassoSmemIo *io, PicassoGlink *s, uint32_t offset, void *out, uint32_t bytes) {
    uint32_t at = (s->rxTail + offset) % s->rx.size;
    uint32_t first = bytes < s->rx.size - at ? bytes : s->rx.size - at;
    if (wlan_smread(&io->reader, s->rx.offset + at, out, first) ||
        (bytes > first && wlan_smread(&io->reader, s->rx.offset,
          static_cast<uint8_t *>(out) + first, bytes - first))) return fail(s, -5);
    return 0;
}
bool sameName(const char *a, const char *b) {
    for (unsigned i = 0; i < 32; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    return false;
}
PicassoGlinkChannel *remoteChannel(PicassoGlink *s, unsigned id) {
    for (auto &c : s->channel) if (c.used && c.remote == id && id) return &c;
    return nullptr;
}
PicassoGlinkChannel *localChannel(PicassoGlink *s, unsigned id) {
    for (auto &c : s->channel) if (c.used && c.local == id && id) return &c;
    return nullptr;
}
void resetIntent(PicassoGlinkLocalIntent &i) {
    i.id=0; i.fill=0; i.left=0; i.advertised=false; i.active=false; i.done=false; i.delivered=false;
}
PicassoGlinkLocalIntent *locals(PicassoGlink *s, const PicassoGlinkChannel &c) {
    return c.local==1 ? s->localIntent : s->qrtrLocal;
}
unsigned localCount(const PicassoGlinkChannel &c) { return c.local==1 ? PicassoGlinkLocalIntents : 10; }
PicassoGlinkRemoteIntent *remotes(PicassoGlink *s, const PicassoGlinkChannel &c) {
    return c.local==1 ? s->remoteIntent : s->qrtrRemote;
}
void clearChannel(PicassoGlink *s, PicassoGlinkChannel &c) {
    if (c.local==1) { s->apr.ready=false; s->apr.pending=false; s->apr.error=-104; }
    else picasso_qrtr_invalidate(&s->qrtr,-104);
    for (unsigned n=0;n<localCount(c);++n) resetIntent(locals(s,c)[n]);
    for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) remotes(s,c)[n]={};
}
void updateChannel(PicassoGlink *s, PicassoGlinkChannel *c) {
    bool up = c->supported && !c->closing && c->openSent && c->openAck && c->ackSent && c->remote;
    if (up != c->up) { c->up = up; ++s->channelEvents; }
}
int channelWork(const PicassoSmemIo *io, PicassoGlink *s, PicassoGlinkChannel &c, bool *kick) {
    if (!s->ready || !c.supported) return 0;
    if (c.closing) return 0;
    int error;
    if (!c.up && ++c.steps > 500) {
        if (c.local==1) s->apr.error = -60; else s->qrtr.error = -60;
        if (c.openSent) {
            const uint32_t words[2] = {3U | (uint32_t(c.local) << 16), 0};
            if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        }
        c.closing = true; ++s->channelEvents; return 0;
    }
    if (!c.openSent) {
        unsigned length=1; while (c.name[length-1]) ++length;
        uint32_t words[10] = {2U | (uint32_t(c.local) << 16), length};
        for (unsigned i = 0; i < length; ++i) words[2 + i / 4] |= uint32_t(uint8_t(c.name[i])) << ((i & 3) * 8);
        if ((error = transmit(io, s, words, 8+((length+7)&~7U), kick))) return error;
        c.openSent = true; ++s->channelEvents;
    }
    if (c.remote && !c.ackSent) {
        const uint32_t words[2] = {4U | (uint32_t(c.remote) << 16), 0};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        c.ackSent = true;
    }
    updateChannel(s, &c);
    if (!c.up) return 0;
    auto &a = s->apr;
    if (c.local==1) ++a.ticks;
    // Count service calls even under TX backpressure. A timeout does not free
    // any remote intent; its ownership lasts until RX_DONE or disconnect.
    if (c.local==1 && !a.ready && !a.error) {
        if (a.pending && ++a.wait > 50) { a.pending = false; a.next = a.ticks + 5; }
        if (a.ticks > 250 || (!a.pending && a.attempts >= 3 && a.ticks >= a.next)) {
            a.pending = false; a.error = -60;
        }
    }
    // Send at most one completion and one advertisement per call. An intent
    // remains occupied until RX_DONE is published; non-reuse IDs never repeat.
    auto *local=locals(s,c); auto *remote=remotes(s,c);
    for (unsigned n=0;n<localCount(c);++n) {
        auto &i=local[n];
        if (!i.done) continue;
        if (!i.delivered) {
            if (c.local==2 && picasso_qrtr_receive(&s->qrtr,i.data,i.fill)==-11) break;
            i.delivered=true;
        }
        const uint32_t words[2] = {6U | (uint32_t(c.local) << 16), i.id};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        resetIntent(i); break;
    }
    for (unsigned n=0;n<localCount(c);++n) {
        auto &i=local[n]; if (i.id) continue;
        if (!s->nextIntent) return fail(s, -75);
        const uint32_t words[4] = {5U | (uint32_t(c.local) << 16), 1, i.capacity, s->nextIntent};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        i.id = s->nextIntent++; i.advertised = true; break;
    }
    if (c.local==2) {
        auto &q=s->qrtr;
        if (q.error || (q.helloSent && !q.count)) return 0;
        PicassoGlinkRemoteIntent *slot=nullptr;
        for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n)
            if (remote[n].used && !remote[n].busy && remote[n].size>=52) { slot=&remote[n]; break; }
        if (!slot) {
            if (!q.intentAsked && !q.intentGranted) {
                const uint32_t words[2]={7U | (uint32_t(c.local)<<16),52};
                if ((error=transmit(io,s,words,sizeof(words),kick))) return error;
                q.intentAsked=true;
            }
            return 0;
        }
        uint32_t words[18]={9U | (uint32_t(c.local)<<16),slot->id,52,0};
        if (!q.helloSent) picasso_qrtr_control(words+4,2,1,PicassoQrtrControl,
            q.helloReceived?q.peer:0xffffffffU,PicassoQrtrControl);
        else for (unsigned n=0;n<13;++n) words[4+n]=q.queue[q.head][n];
        if ((error=transmit(io,s,words,sizeof(words),kick))) return error;
        slot->busy=true; q.intentGranted=false;
        if (!q.helloSent) q.helloSent=true;
        else { q.head=(q.head+1)%PicassoQrtrQueue; --q.count; ++q.resumes; }
        ++q.events; return 0;
    }
    if (a.ready || a.error) return 0;
    if (a.pending || a.ticks < a.next) return 0;
    PicassoGlinkRemoteIntent *slot = nullptr;
    for (auto &i : s->remoteIntent) if (i.used && !i.busy && i.size >= 20) { slot = &i; break; }
    if (!slot) {
        if (!a.intentAsked && !a.intentGranted) {
            const uint32_t words[2] = {7U | (uint32_t(c.local) << 16), 20};
            if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
            a.intentAsked = true;
        }
        return 0;
    }
    uint32_t words[10] = {9U | (uint32_t(c.local) << 16), slot->id, 20, 0};
    uint32_t query[5]; picasso_apr_query(a.token + 1, query);
    for (unsigned i = 0; i < 5; ++i) words[4 + i] = query[i];
    if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
    slot->busy = true; ++a.token; ++a.attempts; a.wait = 0; a.pending = true;
    a.intentGranted = false;
    return 0;
}
int receivePipe(const PicassoSmemIo *io, PicassoGlink *s) {
    struct wlan_smem_partition p = {}; wlan_smem_item desc = {}, tx = {}, rx = {};
    int error = wlan_smem_partition(&io->reader, 0, 2, &p);
    if (error) return error;
    if ((error = wlan_smem_find(&io->reader, &p, 478, &desc)) ||
        (error = wlan_smem_find(&io->reader, &p, 479, &tx))) return error;
    if (desc.offset != s->desc.offset || desc.size != s->desc.size ||
        tx.offset != s->tx.offset || tx.size != s->tx.size) return -22;
    if ((error = wlan_smem_find(&io->reader, &p, 480, &rx))) return error;
    // The remote owns FIFO_1 and declares its length (Linux does not fix it to
    // APPS' 16 KiB). Reject impossible or changing pipe geometry.
    if (rx.size < 16 || (rx.size & 7) || rx.size > 65536 ||
        (s->rx.size && (s->rx.offset != rx.offset || s->rx.size != rx.size))) return -22;
    s->rx = rx; return 0;
}
// A complete command is committed only after all required responses have room.
// Unsupported channel names are retained without OPEN_ACK or receive buffers;
// they do not prevent unrelated services sharing this FIFO from making progress.
int receive(const PicassoSmemIo *io, PicassoGlink *s, unsigned command, unsigned id,
            uint32_t data, uint32_t avail, uint32_t *length, unsigned *intentBudget, bool *kick) {
    if (!s->ready) return fail(s, -71);
    auto *c = remoteChannel(s, id);
    int error;
    if (command == 2) {
        unsigned nameBytes = data & 0xffff;
        if (!id || nameBytes < 2 || nameBytes > 32) return fail(s, -22);
        *length = 8 + ((nameBytes + 7) & ~7U);
        if (*length > s->rx.size - 8) return fail(s, -90);
        if (avail < *length) return -11;
        char name[32] = {};
        if ((error = peek(io, s, 8, name, nameBytes))) return error;
        if (name[nameBytes - 1]) return fail(s, -22);
        for (unsigned i = 0; i < nameBytes - 1; ++i)
            if (uint8_t(name[i]) < 0x21 || uint8_t(name[i]) > 0x7e) return fail(s, -22);
        if (c) return fail(s, -17); // an rcid cannot change owners mid-session
        for (auto &candidate : s->channel)
            if (candidate.used && sameName(candidate.name, name)) { c = &candidate; break; }
        if (c && (c->remote || c->closing)) return fail(s, -17);
        if (!c) {
            for (auto &candidate : s->channel) if (!candidate.used) { c = &candidate; break; }
            if (!c) return fail(s, -28);
            *c = {}; c->used = true;
            for (unsigned i = 0; i < nameBytes; ++i) c->name[i] = name[i];
            if (sameName(name,"IPCRTR")) { c->supported=true; c->local=2; }
            else ++s->deferredOpens;
        }
        c->remote = uint16_t(id); c->priority = uint16_t(data >> 16); ++s->channelEvents;
        return 0;
    }
    if (command == 4) {
        c = localChannel(s, id);
        if (!c || !c->openSent || c->openAck || c->closing || data) return fail(s, -71);
        c->openAck = true; updateChannel(s, c); ++s->channelEvents; return 0;
    }
    if (command == 3) {
        if (!c || data) return fail(s, -71);
        uint32_t words[4] = {11U | (id << 16), 0, 3U | (uint32_t(c->local) << 16), 0};
        if ((error = transmit(io, s, words, c->openSent && !c->closing ? 16 : 8, kick))) return error;
        if (c->supported) { c->closing = true; c->remote = 0; c->up = false; clearChannel(s,*c); }
        else *c = {};
        ++s->channelEvents; return 0;
    }
    if (command == 11) {
        c = localChannel(s, id);
        if (!c || !c->closing || c->closeAck || data) return fail(s, -71);
        c->closeAck = true; ++s->channelEvents; return 0;
    }
    if (!c) return fail(s, -71);
    if (command == 15) { c->signals = data; return 0; }
    if (command == 5) {
        if (!c->supported || !c->ackSent || c->closing) {
            s->intentFault = 1;
            return fail(s, -22);
        }
        if (s->intentProgress && (s->intentCount != data || s->intentTail != s->rxTail || s->intentChannel != id)) {
            s->intentFault = 3; return fail(s, -71);
        }
        // Wire count is bounded by the FIFO, not our cache capacity. The DSP
        // may advertise more buffers than this bounded bring-up client needs.
        if (data > (s->rx.size - 16) / 8) { s->intentFault = 2; return fail(s, -90); }
        *length = 8 + data * 8;
        if (avail < *length) return -11;
        s->intentCount = data; s->intentTail = s->rxTail; s->intentChannel = id;
        // At most sixteen descriptors per service call. Keep RX tail until the
        // full command is parsed; the peer cannot reuse its payload meanwhile.
        while (s->intentProgress < data && *intentBudget) {
            uint8_t pair[8];
            if ((error = peek(io, s, 8 + s->intentProgress * 8, pair, sizeof(pair)))) return error;
            uint32_t size = wlan_sm32(pair), iid = wlan_sm32(pair + 4);
            s->lastIntentId = iid; s->lastIntentSize = size;
            // Local IDs start at one by policy. Remote IDs are opaque u32s:
            // stock idr_alloc(riids, ..., iid, iid+1) explicitly accepts zero.
            if (!size) { s->intentFault = 4; return fail(s, -22); }
            PicassoGlinkRemoteIntent *slot = nullptr;
            for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
                auto &i=remotes(s,*c)[n];
                if (i.used && i.id == iid) { s->intentFault = 5; return fail(s, -17); }
                if (!i.used && !slot) slot = &i;
            }
            if (!slot) {
                for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
                    auto &i=remotes(s,*c)[n];
                    if (!i.busy && i.size < size && (!slot || i.size < slot->size)) slot = &i;
                }
                if (slot) ++s->intentReplaced;
            }
            // Idle descriptors are remote-owned storage, not allocations held
            // by APPS. We may leave them unused. Never evict an in-flight ID or
            // send RX_DONE for an ignored advertisement. Keep a larger grant.
            if (slot) { slot->id = iid; slot->size = size; slot->used = true; }
            else ++s->intentSkipped;
            ++s->intentOffers; ++s->intentProgress; --*intentBudget;
        }
        if (s->intentProgress < data) return -11;
        s->intentProgress = 0; ++s->intentFrames;
        return 0;
    }
    if (command == 7) {
        if (!c->up || !data) return fail(s, -71);
        PicassoGlinkLocalIntent *slot = nullptr;
        for (unsigned n=0;n<localCount(*c);++n) {
            auto &i=locals(s,*c)[n]; if (!i.id && i.capacity>=data) { slot=&i; break; }
        }
        if (slot && !s->nextIntent) return fail(s, -75);
        uint32_t words[6] = {5U | (uint32_t(c->local) << 16), 1, slot ? slot->capacity : 0, s->nextIntent,
                            8U | (uint32_t(c->local) << 16), slot ? 1U : 0U};
        if ((error = transmit(io, s, slot ? words : words + 4, slot ? 24 : 8, kick))) return error;
        if (slot) { slot->id = s->nextIntent++; slot->advertised = true; }
        return 0;
    }
    if (command == 8) {
        auto &asked=c->local==1 ? s->apr.intentAsked : s->qrtr.intentAsked;
        auto &granted=c->local==1 ? s->apr.intentGranted : s->qrtr.intentGranted;
        if (!c->up || !asked || data > 1) return fail(s, -71);
        granted=data!=0 && (c->local!=1 || !s->apr.pending); asked=false;
        if (!data) {
            if (c->local==1 && !s->apr.pending) s->apr.error=-28;
            if (c->local==2) s->qrtr.error=-28;
        }
        return 0;
    }
    if (command == 6) {
        if (!c->up) return fail(s, -71);
        for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
            auto &i=remotes(s,*c)[n];
            if (i.used && i.id == data && i.busy) { i = {}; return 0; }
        }
        return fail(s, -71);
    }
    if (command == 9 || command == 12) {
        if (!c->up) return fail(s, -71);
        if (avail < 16) return -11;
        uint32_t sizes[2];
        if ((error = peek(io, s, 8, sizes, 8))) return error;
        uint32_t chunk = sizes[0], left = sizes[1];
        if (!chunk || chunk>0x4400 || left>0x4400-chunk) return fail(s,-90);
        *length = 16 + ((chunk + 7) & ~7U);
        if (*length > s->rx.size - 8) return fail(s, -90);
        if (avail < *length) return -11;
        PicassoGlinkLocalIntent *slot = nullptr;
        for (unsigned n=0;n<localCount(*c);++n) {
            auto &i=locals(s,*c)[n]; if (i.id==data && data) { slot=&i; break; }
        }
        if (!slot || !slot->advertised || slot->done ||
            (command == 9 ? slot->active : !slot->active)) return fail(s, -71);
        if (chunk>slot->capacity || left>slot->capacity-chunk ||
            slot->fill > slot->capacity - chunk - left ||
            (command == 12 && slot->left != chunk + left)) return fail(s, -90);
        if ((error = peek(io, s, 16, slot->data + slot->fill, chunk))) return error;
        slot->fill += chunk; slot->left = left; slot->active = left != 0;
        if (!left) {
            if (c->local==1) { picasso_apr_receive(&s->apr,slot->data,slot->fill); slot->delivered=true; }
            else slot->delivered=picasso_qrtr_receive(&s->qrtr,slot->data,slot->fill)!=-11;
            slot->done = true; slot->advertised = false; ++s->dataPackets;
        }
        return 0;
    }
    // Unknown commands cannot be skipped without knowing their framing.
    // In particular reuse (14) was not negotiated.
    return fail(s, -95);
}
}
int picasso_glink_prepare(const PicassoSmemIo *io, PicassoGlink *s, PicassoSmemAudit *audit) {
    if (!s) return -22;
    // Aggregate assignment creates a large temporary at -O0, overflowing a
    // small kernel stack despite the caller using static storage. Zero in place.
    auto *bytes = reinterpret_cast<uint8_t *>(s);
    for (unsigned i = 0; i < sizeof(*s); ++i) bytes[i] = 0;
    int error = picasso_smem_prepare_glink(io, &s->desc, &s->tx, audit);
    if (error) return fail(s, error);
    auto &c = s->channel[0]; c.used = true; c.supported = true; c.local = 1;
    const char name[] = "apr_audio_svc";
    for (unsigned i = 0; i < sizeof(name); ++i) c.name[i] = name[i];
    for (unsigned n=0;n<PicassoGlinkLocalIntents;++n) {
        s->localIntent[n].data=s->aprStorage[n]; s->localIntent[n].capacity=PicassoGlinkPacket;
    }
    unsigned offset=0;
    for (unsigned n=0;n<10;++n) {
        auto &i=s->qrtrLocal[n]; i.capacity=n<5 ? 0x800 : n<8 ? 0x2000 : 0x4400;
        i.data=s->qrtrStorage+offset; offset+=i.capacity;
    }
    s->nextIntent = 1; s->apr.token = 0x18300000;
    s->prepared = true; return 0;
}
int picasso_glink_service(const PicassoSmemIo *io, PicassoGlink *s, bool *kick) {
    if (!io || !io->reader.read || !io->write32 || !io->barrier || !s || !kick) return -22;
    *kick = false;
    if (!s->prepared || s->failed) return s->error ? s->error : -22;
    if (!s->ready && ++s->steps > 500) return fail(s, -60);
    int error = 0;
    if (!s->sent) {
        if ((error = send(io, s, 0, kick))) return error;
        s->sent = true; // version 1, features 0: no unimplemented intent reuse
    }
    error = receivePipe(io, s);
    if (error == -2 || error == -11) return 0;
    if (error) return fail(s, error);
    // Only work already due at entry is sent here. Newly received OPENs are
    // acknowledged on the next service tick, after the command is committed.
    // Descriptors from a partially parsed command are not eligible for TX.
    if (!s->intentProgress) for (auto &c:s->channel) if (c.used && c.supported) {
        error=channelWork(io,s,c,kick); if (s->failed) return error;
    }
    unsigned intentBudget = 16;
    for (unsigned budget = 0; budget < 8; ++budget) {
        uint32_t head = 0, tail = 0, words[2] = {};
        if (rd(io, s->desc.offset + 12, &head) || rd(io, s->desc.offset + 8, &tail)) return fail(s, -5);
        if (head >= s->rx.size || (head & 7) || tail != s->rxTail) return fail(s, -22);
        if (head == tail) break;
        fence(io); // acquire peer's payload after its atomic RX head
        if (rd(io, s->rx.offset + tail, words) || rd(io, s->rx.offset + tail + 4, words + 1)) return fail(s, -5);
        unsigned command = words[0] & 0xffff, version = words[0] >> 16;
        uint32_t length = 8, avail = (head + s->rx.size - tail) % s->rx.size;
        if (s->intentProgress && command != 5) { s->intentFault = 3; return fail(s, -71); }
        if (command == 0 || command == 1) {
            if (!version) return fail(s, -93);
            if (s->retries >= 16) return fail(s, -71);
            if (command == 0) {
                if ((error = send(io, s, 1, kick))) return error;
                if (version == 1) s->peerVersion = true;
            } else if (version == 1 && words[1] == 0) s->peerAck = true;
            else if ((error = send(io, s, 0, kick))) return error;
            ++s->retries;
        } else if (command == 13) {
            *kick = true; // READ_NOTIF: notify peer after advancing our RX tail
        } else {
            s->pendingCommand = uint16_t(command);
            s->pendingChannel = uint16_t(version); s->pendingData = words[1];
            error = receive(io, s, command, version, words[1], avail, &length, &intentBudget, kick);
            if (error == -11) return 0; // incomplete frame or TX pressure; retry without consuming
            if (error) return error;
        }
        uint32_t next = (tail + length) % s->rx.size;
        fence(io);
        if ((error = wr(io, s, s->desc.offset + 8, next))) return error;
        fence(io); s->rxTail = next; ++s->commands;
        s->ready = s->peerVersion && s->peerAck;
        if (!intentBudget) break;
    }
    return 0;
}
void picasso_glink_stop(PicassoGlink *s) {
    if (!s) return;
    fail(s, -104);
    for (auto &c:s->channel) if (c.used && c.supported) clearChannel(s,c);
}
