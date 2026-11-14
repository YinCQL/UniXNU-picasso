// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent implementation of the Qualcomm QMP wire protocol, checked
// against the Picasso stock kernel (msm_qmp.c). No Linux code is copied here.
#include "PicassoQmp.h"

namespace {
constexpr uint32_t up = 0x0000ffff, down = 0xffff0000, messageBytes = 96;
enum Word { Magic, Version, Features, RemoteLink, RemoteLinkAck, RemoteChannel,
    RemoteChannelAck, RxSize, RxOffset, LocalLink, LocalLinkAck, LocalChannel,
    LocalChannelAck, TxSize, TxOffset };
uint32_t read(PicassoQmp *q, unsigned word) { return q->io.read(q->io.context, word * 4); }
void write(PicassoQmp *q, unsigned word, uint32_t value) { q->io.write(q->io.context, word * 4, value); }
void fence(PicassoQmp *q) { q->io.barrier(q->io.context); }
void kick(PicassoQmp *q) { fence(q); q->io.kick(q->io.context); }
bool window(uint32_t offset, uint32_t bytes, uint32_t ram, uint32_t minimum) {
    return !(offset & 3) && !(bytes & 3) && offset >= 64 && offset <= ram &&
        bytes >= minimum && bytes <= ram - offset;
}
bool snapshot(PicassoQmp *q, bool initial = false) {
    fence(q);
    for (unsigned i = 0; i < 16; ++i) q->descriptor[i] = read(q, i);
    fence(q);
    const uint32_t *d = q->descriptor;
    if (d[Magic] != 0x4d41494c || d[Version] != 1 || d[Features] != 0 ||
        !window(d[RxOffset], d[RxSize], q->ramBytes, 4) ||
        !window(d[TxOffset], d[TxSize], q->ramBytes, messageBytes + 4) ||
        (d[RxOffset] < d[TxOffset] + d[TxSize] && d[TxOffset] < d[RxOffset] + d[RxSize])) return false;
    // Recheck immutable identity/layout before using offsets, including changes
    // after initialization. A restarted peer must not redirect writes elsewhere.
    const unsigned identity[] = {Magic, Version, Features, RxSize, RxOffset, TxSize, TxOffset};
    for (unsigned i : identity)
        if (d[i] != read(q, i)) return false;
    if (!initial && (d[RxOffset] != q->rx || d[RxSize] != q->rxBytes ||
                    d[TxOffset] != q->tx || d[TxSize] != q->txBytes)) return false;
    // Zero is the uninitialized boot descriptor state. Never acknowledge it as up.
    for (unsigned i = RemoteLink; i <= LocalChannelAck; ++i) {
        if (i == RxSize || i == RxOffset) continue;
        if (d[i] != 0 && d[i] != up && d[i] != down) return false;
    }
    return true;
}
int pending(PicassoQmp *q) {
    // No ownership of previous-stage traffic or unsolicited receive messages.
    if (q->io.read(q->io.context, q->tx)) return QmpBusy;
    if (q->io.read(q->io.context, q->rx)) return QmpReceivePending;
    return 0;
}
void acknowledge(PicassoQmp *q) {
    bool changed = false;
    const unsigned states[] = {RemoteLink, RemoteChannel};
    for (unsigned word : states) {
        uint32_t value = q->descriptor[word];
        if ((value == up || value == down) && q->descriptor[word + 1] != value) {
            write(q, word + 1, value); q->descriptor[word + 1] = value; changed = true;
        }
    }
    if (changed) kick(q);
}
bool linked(const PicassoQmp *q) {
    const uint32_t *d = q->descriptor;
    return d[RemoteLink] == up && d[RemoteLinkAck] == up &&
        d[LocalLink] == up && d[LocalLinkAck] == up;
}
bool open(const PicassoQmp *q) {
    const uint32_t *d = q->descriptor;
    return linked(q) && d[RemoteChannel] == up && d[RemoteChannelAck] == up &&
        d[LocalChannel] == up && d[LocalChannelAck] == up;
}
}

int picasso_qmp_begin(PicassoQmp *q, const QmpIo *io, uint32_t bytes, uint64_t deadline) {
    if (!q || !io || !io->read || !io->write || !io->barrier || !io->kick || bytes < 64 || (bytes & 3))
        return QmpDescriptor;
    *q = {}; q->io = *io; q->ramBytes = bytes; q->deadline = deadline;
    if (!snapshot(q, true)) return q->state = QmpDescriptor;
    q->rx = q->descriptor[RxOffset]; q->rxBytes = q->descriptor[RxSize];
    q->tx = q->descriptor[TxOffset]; q->txBytes = q->descriptor[TxSize];
    int error = pending(q);
    if (error) return q->state = error;
    // Normalize even an inherited idle/open channel through an acknowledged
    // down -> up transition. Old UP bits alone never count as a fresh handshake.
    q->owned = true;
    acknowledge(q);
    write(q, LocalChannel, down);
    write(q, LocalLink, up);
    kick(q);
    return q->state = QmpLinkWait;
}

int picasso_qmp_step(PicassoQmp *q, uint64_t now) {
    if (q->state <= QmpClosed) return q->state;
    if (!snapshot(q)) return q->state = QmpDescriptor;
    if (now >= q->deadline) return q->state = QmpTimeout;
    const uint32_t *d = q->descriptor;
    // Check loss of the peer before interpreting a cleared length as completion.
    if ((q->state == QmpOpen || q->state == QmpSending) && !open(q)) {
        acknowledge(q);
        return q->state = QmpDisconnected;
    }
    acknowledge(q);
    if (q->io.read(q->io.context, q->rx)) return q->state = QmpReceivePending;
    if (q->state == QmpLinkWait) {
        if (!linked(q) || d[LocalChannelAck] != down) return q->state;
        if (q->io.read(q->io.context, q->tx)) return q->state = QmpBusy;
        write(q, LocalChannel, up); kick(q);
        return q->state = QmpChannelWait;
    }
    if (q->state == QmpChannelWait) {
        if (!linked(q)) return q->state = QmpDisconnected;
        if (!open(q)) return q->state;
        int error = pending(q);
        return q->state = error ? error : QmpOpen;
    }
    if (q->state == QmpClosing) {
        if (!linked(q)) return q->state = QmpDisconnected;
        if (d[LocalChannel] == down && d[LocalChannelAck] == down) {
            q->owned = false; return q->state = QmpClosed;
        }
    }
    if (q->state == QmpSending && !q->io.read(q->io.context, q->tx)) q->state = QmpOpen;
    return q->state;
}

int picasso_qmp_send(PicassoQmp *q, const char *text, uint64_t deadline) {
    if (q->state != QmpOpen || !text) return QmpBusy;
    uint32_t words[messageBytes / 4] = {};
    unsigned n = 0;
    for (; n < messageBytes && text[n]; ++n)
        words[n / 4] |= uint32_t(uint8_t(text[n])) << (8 * (n % 4));
    if (!n || n == messageBytes) return QmpTooLong;
    if (!snapshot(q)) return q->state = QmpDescriptor;
    if (!open(q)) return q->state = QmpDisconnected;
    int error = pending(q);
    if (error) return q->state = error;
    for (unsigned i = 0; i < messageBytes / 4; ++i) q->io.write(q->io.context, q->tx + 4 + i * 4, words[i]);
    fence(q); // Payload -> length -> doorbell, 32-bit accesses throughout.
    q->io.write(q->io.context, q->tx, messageBytes);
    kick(q);
    q->deadline = deadline;
    return q->state = QmpSending;
}

int picasso_qmp_close(PicassoQmp *q, uint64_t deadline) {
    if (!q->owned) return q->state;
    if (!snapshot(q)) return q->state = QmpDescriptor;
    int error = pending(q);
    if (error) return q->state = error; // Never erase an unconsumed message, even on timeout.
    acknowledge(q);
    write(q, LocalChannel, down); kick(q);
    q->deadline = deadline;
    return q->state = QmpClosing;
}
