// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire facts: Picasso stock qcom_glink_{smem,native}.c. Independent implementation.
#include "PicassoGlink.h"
namespace {
int fail(PicassoGlink *s, int error) { s->failed = true; s->error = error; return error; }
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
int send(const PicassoSmemIo *io, PicassoGlink *s, unsigned command, bool *kick) {
    uint32_t tail = 0, head = 0;
    if (rd(io, s->desc.offset, &tail) || rd(io, s->desc.offset + 4, &head)) return fail(s, -5);
    if (head != s->txHead || tail >= s->tx.size || (tail & 7)) return fail(s, -22);
    uint32_t free = tail > head ? tail - head : s->tx.size - head + tail;
    // Stock reserves eight bytes to distinguish empty/full and another eight
    // for READ_NOTIF. Do not block the boot/fatal worker waiting for TX space.
    if (free < 24) return -11;
    fence(io); // acquire peer's released slots before reusing their payload
    int error = wr(io, s, s->tx.offset + head, command | (1U << 16));
    if (error) return error;
    if ((error = wr(io, s, s->tx.offset + head + 4, 0))) return error;
    fence(io); // publish payload before APPS TX head
    uint32_t next = (head + 8) % s->tx.size;
    if ((error = wr(io, s, s->desc.offset + 4, next))) return error;
    fence(io); s->txHead = next; *kick = true;
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
}
int picasso_glink_prepare(const PicassoSmemIo *io, PicassoGlink *s, PicassoSmemAudit *audit) {
    if (!s) return -22;
    *s = {};
    int error = picasso_smem_prepare_glink(io, &s->desc, &s->tx, audit);
    if (error) return fail(s, error);
    s->prepared = true; return 0;
}
int picasso_glink_service(const PicassoSmemIo *io, PicassoGlink *s, bool *kick) {
    if (!io || !io->reader.read || !io->write32 || !io->barrier || !s || !kick) return -22;
    *kick = false;
    if (!s->prepared || s->failed) return s->error ? s->error : -22;
    if (s->blocked) return 0;
    if (!s->ready && ++s->steps > 500) return fail(s, -60);
    int error = 0;
    if (!s->sent) {
        if ((error = send(io, s, 0, kick))) return error;
        s->sent = true; // version 1, features 0: no unimplemented intent reuse
    }
    error = receivePipe(io, s);
    if (error == -2 || error == -11) return 0;
    if (error) return fail(s, error);
    for (unsigned budget = 0; budget < 8; ++budget) {
        uint32_t head = 0, tail = 0, words[2] = {};
        if (rd(io, s->desc.offset + 12, &head) || rd(io, s->desc.offset + 8, &tail)) return fail(s, -5);
        if (head >= s->rx.size || (head & 7) || tail != s->rxTail) return fail(s, -22);
        if (head == tail) break;
        fence(io); // acquire peer's payload after its atomic RX head
        if (rd(io, s->rx.offset + tail, words) || rd(io, s->rx.offset + tail + 4, words + 1)) return fail(s, -5);
        unsigned command = words[0] & 0xffff, version = words[0] >> 16;
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
            // Preserve the full next command for the future channel owner.
            // In particular do not ACK OPEN, advertise intents, discard data
            // or claim apr_audio_svc is open without an implemented consumer.
            s->blocked = true; s->pendingCommand = uint16_t(command);
            s->pendingChannel = uint16_t(version); s->pendingData = words[1];
            return 0;
        }
        uint32_t next = (tail + 8) % s->rx.size;
        fence(io);
        if ((error = wr(io, s, s->desc.offset + 8, next))) return error;
        fence(io); s->rxTail = next; ++s->commands;
        s->ready = s->peerVersion && s->peerAck;
    }
    return 0;
}
