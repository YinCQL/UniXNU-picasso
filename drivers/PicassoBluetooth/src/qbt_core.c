/* SPDX-License-Identifier: GPL-2.0-only */
/* H4 framing and Qualcomm IBS, following Linux hci_qca and h4_recv_buf.
 * Byte storage is bounded. The executor sleeps on actual work/deadline events. */
#include "qbt_internal.h"
#include <stdarg.h>
#define SLEEP_IND 0xfe
#define WAKE_IND 0xfd
#define WAKE_ACK 0xfc
static uint64_t now(struct qbt *q) { return q->ops.now_ns(q->ops.context); }
static bool drained(struct qbt *q) { return q->ops.uart_drained(q->ops.context); }
static int clock_on(struct qbt *q) {
    if (q->clock_on) return 0;
    int e = q->ops.clock(q->ops.context, true);
    if (!e) q->clock_on = true;
    return e;
}
static bool flush(struct qbt *q) {
    while (q->forward_offset < q->forward_length) {
        size_t n = q->ops.receive(q->ops.context, q->forward + q->forward_offset,
                                q->forward_length - q->forward_offset);
        if (!n) { q->stats.backpressure++; return false; }
        if (n > q->forward_length - q->forward_offset) n = q->forward_length - q->forward_offset;
        q->forward_offset += (unsigned)n; q->stats.delivered_bytes += n;
    }
    q->forward_offset = q->forward_length = 0;
    return true;
}
static void control(struct qbt *q, uint8_t b) {
    if (q->control_length < sizeof(q->control)) q->control[q->control_length++] = b;
    else q->stats.malformed++;
}
/* Stop each DMA chunk at a packet boundary. IBS bytes may never be inserted
 * into the middle of a command, ACL, SCO or ISO frame spanning several slots. */
static size_t tx_advance(struct qbt_tx_frame *f,const uint8_t *p,size_t n) {
    size_t i=0;
    while(i<n) {
        if(!f->type) {
            uint8_t t=p[i++];if(t!=1 && t!=2 && t!=3 && t!=5)return 0;
            f->type=t;f->have=0;f->need=t==1||t==3?3:4;
        } else if(f->need) {
            f->header[f->have++]=p[i++];if(--f->need)continue;
            f->body=f->type==1||f->type==3?f->header[2]:
                (f->header[2]|(unsigned)f->header[3]<<8)&(f->type==5?0x3fff:0xffff);
            if(!f->body){f->type=0;return i;}
        } else {
            size_t take=n-i;if(take>f->body)take=f->body;
            i+=take;f->body-=(unsigned)take;if(!f->body){f->type=0;return i;}
        }
    }
    return i;
}
static void event(struct qbt *q) {
    const uint8_t *e = q->event; unsigned n = q->event_length;
    if (n >= 3 && e[0] == 0x10) { q->stats.hardware_errors++; q->stats.last_hardware_error = e[2]; }
    /* The speed change completes after the switch, at the new rate. */
    if (n >= 5 && e[0] == 0x0e && e[3] == 0x48 && e[4] == 0xfc) q->speed_reply = true;
    /* A nonzero TLV report describes an earlier segment; only status 0 answers a wait. */
    if (n >= 7 && e[0] == 0x0e && e[3] == 0x00 && e[4] == 0xfc && e[6] == 0x1e) {
        q->stats.tlv_acks++; q->stats.tlv_final_status = e[5];
        if (e[5]) return;
    }
    if (q->waiting && !q->complete) {
        bool match = false;
        if (q->wait_event == QBT_VERSION_EVENT) {
            /* Older ROMs may acknowledge the command before sending their VSE.
             * A successful status-only CC is not the version response. */
            if (n >= 6 && e[0] == 0x0e && (e[3] | (unsigned)e[4] << 8) == q->wait_opcode)
                match = e[5] != 0 || n > 6;
            else if (e[0] == 0xff) match = true;
        } else if (!q->wait_event && n >= 5 && e[0] == 0x0e)
            match = (e[3] | (unsigned)e[4] << 8) == q->wait_opcode;
        else if (q->wait_event == e[0]) match = true;
        if (match) {
            /* Matching replies have independent storage from parser scratch. */
            memcpy(q->reply, e, n); q->reply_length = n; q->complete = true;
            return;
        }
        if (n >= 6 && e[0] == 0x0f && (e[4] | (unsigned)e[5] << 8) == q->wait_opcode && e[2]) {
            memcpy(q->reply, e, n); q->reply_length = n; q->complete = true;
            return;
        }
    }
    if (q->service && !q->drop) {
        q->forward[0] = 4; memcpy(q->forward + 1, e, n);
        q->forward_length = n + 1;
    } else q->stats.unmatched_events++;
}
struct qbt *qbt_create(const struct qbt_platform *p) {
    if (!p || !p->allocate || !p->release || !p->now_ns || !p->uart_write ||
        !p->uart_drained || !p->uart_speed || !p->uart_flow || !p->clock ||
        !p->pump || !p->wait_until || !p->receive || !p->firmware_get || !p->firmware_put) return NULL;
    struct qbt *q = p->allocate(p->context, sizeof(*q));
    if (q) { memset(q, 0, sizeof(*q)); q->ops = *p; q->clock_on = true; q->info.baud = 115200; }
    return q;
}
void qbt_reset(struct qbt *q) {
    /* Executor only, after board code has stopped/reset DMA and the controller. */
    struct qbt_platform ops=q->ops;struct qbt_stats stats=q->stats;
    memset(q,0,sizeof(*q));q->ops=ops;q->stats=stats;q->clock_on=true;q->info.baud=115200;
}
void qbt_destroy(struct qbt *q) {
    if (!q) return;
    struct qbt_platform p = q->ops; memset(q, 0, sizeof(*q)); p.release(p.context, q, sizeof(*q));
}
size_t qbt_receive(struct qbt *q, const uint8_t *data, size_t length) {
    if (q->info.state == 3) return 0;
    size_t i = 0;
    while (i < length) {
        if (!flush(q)) break;
        if (!q->header_need && !q->body) {
            uint8_t b = data[i++];
            if (b == WAKE_IND) {
                int e = clock_on(q);
                if (e) { qbt_fail(q, e); break; }
                q->rx_awake = true; control(q, WAKE_ACK); q->stats.wake_ack++;
            } else if (b == WAKE_ACK) {
                if (q->tx_state == QBT_WAKING) { q->tx_state = QBT_AWAKE; q->retry_at = 0; q->idle_at = now(q) + QBT_TX_IDLE; }
            } else if (b == SLEEP_IND) q->rx_awake = false;
            else if (b >= 2 && b <= 5) {
                q->type = b; q->header_length = 0; q->header_need = b == 4 ? 2 : b == 3 ? 3 : 4;
                q->drop = !q->service;
            } else { q->stats.malformed++; i = length; }
            continue;
        }
        if (q->header_need) {
            q->header[q->header_length++] = data[i++];
            if (--q->header_need) continue;
            uint8_t t = q->type;
            q->body = t == 4 ? q->header[1] : t == 3 ? q->header[2] :
                (q->header[2] | (unsigned)q->header[3] << 8) & (t == 5 ? 0x3fff : 0xffff);
            q->drop = q->drop || !q->service || (t == 2 &&
                (q->header[0] | (unsigned)q->header[1] << 8) == 0x2edc);
            if (t == 4) { memcpy(q->event, q->header, 2); q->event_length = 2; }
            else if (!q->drop) { q->forward[0] = t; memcpy(q->forward + 1, q->header, q->header_length); q->forward_length = q->header_length + 1; }
            if (!q->body && t == 4) event(q);
            continue;
        }
        size_t n = length - i;
        if (n > q->body) n = q->body;
        if (q->type == 4) {
            if (n > sizeof(q->event) - q->event_length) { q->stats.malformed++; break; }
            memcpy(q->event + q->event_length, data + i, n); q->event_length += (unsigned)n;
        } else if (!q->drop) {
            if (n > sizeof(q->forward)) n = sizeof(q->forward);
            memcpy(q->forward, data + i, n); q->forward_length = (unsigned)n;
        }
        i += n; q->body -= (unsigned)n;
        if (!q->body && q->type == 4) event(q);
    }
    q->stats.rx_bytes += i; (void)flush(q); return i;
}
int qbt_transmit(struct qbt *q, const uint8_t *p, size_t n) {
    if (!q->service || q->info.state != 2) return -ENETDOWN;
    if (!n || !p || n > QBT_TX_CAPACITY - q->tx_length) { q->stats.tx_rejected++; return -ENOBUFS; }
    unsigned tail = (q->tx_head + q->tx_length) % QBT_TX_CAPACITY;
    size_t first = QBT_TX_CAPACITY - tail; if (first > n) first = n;
    memcpy(q->tx + tail, p, first); memcpy(q->tx, p + first, n - first);
    q->tx_length += (unsigned)n; q->stats.host_bytes += n; return 0;
}
size_t qbt_transmit_capacity(const struct qbt *q) {
    return q->service && q->info.state==2?QBT_TX_CAPACITY-q->tx_length:0;
}
void qbt_step(struct qbt *q) {
    if (q->info.state == 3) return;
    (void)flush(q);
    uint64_t t = now(q); bool pending = q->command_length || (q->service && q->tx_length);
    if (pending) {
        int e = clock_on(q);
        if (e) { qbt_fail(q, e); return; }
    }
    if (q->ibs && pending && q->tx_state == QBT_ASLEEP) {
        q->tx_state = QBT_WAKING; q->wake_attempts = 1; control(q, WAKE_IND);
        q->retry_at = t + QBT_WAKE_RETRY; q->stats.wake_ind++;
    } else if (q->ibs && q->tx_state == QBT_WAKING && t >= q->retry_at) {
        if (++q->wake_attempts > 10) {
            q->info.error = -ETIMEDOUT; q->info.state = 3; q->service = false;
            q->retry_at = 0; q->stats.timeouts++; return;
        }
        control(q, WAKE_IND); q->retry_at = t + QBT_WAKE_RETRY; q->stats.wake_retries++;
    }
    if (q->control_length && q->clock_on && !q->tx_frame.type) {
        size_t n = q->ops.uart_write(q->ops.context, q->control, q->control_length);
        if (n > q->control_length) n = q->control_length;
        memmove(q->control, q->control + n, q->control_length - n); q->control_length -= (unsigned)n;
        if (q->control_length) return;
    }
    if (!q->ibs || q->tx_state == QBT_AWAKE) {
        const uint8_t *p = NULL; size_t n = 0; bool cmd = q->command_length != 0;
        if (cmd) { p = q->command + q->command_offset; n = q->command_length - q->command_offset; }
        else if (q->service && q->tx_length) { p = q->tx + q->tx_head; n = QBT_TX_CAPACITY - q->tx_head; if (n > q->tx_length) n = q->tx_length; }
        if (n) {
            if (n > QBT_CHUNK) n = QBT_CHUNK;
            struct qbt_tx_frame preview=q->tx_frame;
            n=tx_advance(&preview,p,n);
            if(!n){q->info.error=-EPROTO;q->info.state=3;q->service=false;return;}
            size_t used = q->ops.uart_write(q->ops.context, p, n); if (used > n) used = n;
            if(used)(void)tx_advance(&q->tx_frame,p,used);
            q->stats.tx_bytes += used;
            if (cmd) { q->command_offset += (unsigned)used; if (q->command_offset == q->command_length) q->command_offset = q->command_length = 0; }
            else { q->tx_head = (q->tx_head + (unsigned)used) % QBT_TX_CAPACITY; q->tx_length -= (unsigned)used; }
            if (used) q->idle_at = t + QBT_TX_IDLE;
        }
        if (q->ibs && !q->command_length && !q->tx_length && !q->control_length &&
            !q->tx_frame.type && q->tx_state == QBT_AWAKE && q->idle_at && t >= q->idle_at && drained(q)) {
            control(q, SLEEP_IND); q->tx_state = QBT_ASLEEP; q->idle_at = 0; q->stats.sleep_ind++;
        }
    }
    if (q->ibs && q->tx_state == QBT_ASLEEP && !q->rx_awake && !q->tx_length &&
        !q->command_length && !q->control_length && !q->tx_frame.type && q->clock_on && drained(q)) {
        if (!q->ops.clock(q->ops.context, false)) { q->clock_on = false; q->stats.clock_sleeps++; }
    }
}
uint64_t qbt_deadline(const struct qbt *q) {
    if (q->info.state == 3) return UINT64_MAX;
    bool idle = q->ops.uart_drained(q->ops.context);
    if (q->control_length && !q->tx_frame.type && idle) return 0;
    if ((q->command_length || (q->service && q->tx_length)) && idle && q->tx_state != QBT_WAKING) return 0;
    if (q->tx_state == QBT_WAKING && q->retry_at) return q->retry_at;
    if (q->ibs && !q->tx_frame.type && q->tx_state == QBT_AWAKE && q->idle_at && idle) return q->idle_at;
    return UINT64_MAX;
}
void qbt_set_service(struct qbt *q, bool enabled) {
    q->service = enabled && q->info.state == 2;
    if (!q->service) {
        if(q->tx_frame.type){q->info.state=3;q->info.error=-ECANCELED;}
        q->drop = true;
        q->tx_head = q->tx_length = 0; q->forward_length = q->forward_offset = 0;
    }
}
void qbt_rx_wake(struct qbt *q) {
    if (q->info.state == 3) return;
    int e = clock_on(q);
    if (e) qbt_fail(q, e);
}
void qbt_fail(struct qbt *q,int error) {
    q->info.state=3;q->info.error=error<0?error:-EIO;q->service=false;
    q->retry_at=q->idle_at=0;
}
static int run(struct qbt *q, uint64_t deadline, unsigned predicate) {
    for (;;) {
        q->ops.pump(q->ops.context); qbt_step(q);
        if (q->info.error) return q->info.error;
        if ((predicate == 0 && !q->command_length) || (predicate == 1 && q->complete) ||
            (predicate == 2 && !q->command_length && drained(q))) return 0;
        uint64_t t = now(q); if (t >= deadline) { q->stats.timeouts++; return -ETIMEDOUT; }
        uint64_t next = qbt_deadline(q);
        /* Wire drain has no IRQ of its own: bytes waiting on it re-check here. */
        if (predicate == 2 || q->command_length || q->control_length) {
            if (!next || next > t + QBT_TX_POLL) next = t + QBT_TX_POLL;
        }
        if (!next || next > deadline) next = deadline;
        /* UART completion and ring-space producers signal this wait. */
        q->ops.wait_until(q->ops.context, next);
    }
}
int qbt_send_command(struct qbt *q, uint16_t op, const void *data, unsigned n) {
    if (n > 255 || (n && !data)) return -EINVAL;
    uint64_t deadline = now(q) + QBT_COMMAND_TIMEOUT;
    int e = run(q, deadline, 0); if (e) return e;
    q->command[0] = 1; q->command[1] = (uint8_t)op; q->command[2] = op >> 8; q->command[3] = (uint8_t)n;
    if (n) memcpy(q->command + 4, data, n);
    q->command_length = n + 4; q->command_offset = 0; q->stats.commands++;
    if (op == 0xfc00 && n && ((const uint8_t *)data)[0] == 0x1e) q->stats.tlv_sent++;
    return run(q, deadline, 0);
}
int qbt_command(struct qbt *q, uint16_t op, const void *data, unsigned n, uint8_t ev,
                uint8_t *reply, size_t *length) {
    if (q->waiting) return -EBUSY;
    q->wait_opcode = op; q->wait_event = ev; q->waiting = true; q->complete = false; q->reply_length = 0;
    uint64_t deadline = now(q) + QBT_COMMAND_TIMEOUT;
    int e = qbt_send_command(q, op, data, n);
    if (!e) e = run(q, deadline, 1);
    q->waiting = false;
    if (e) return e;
    unsigned skip = q->reply[0] == 0x0e ? 5 : 2;
    if (q->reply[0] == 0x0f) return -EIO;
    if (q->reply_length < skip || *length < q->reply_length - skip) return -EMSGSIZE;
    *length = q->reply_length - skip; memcpy(reply, q->reply + skip, *length); return 0;
}
int qbt_drain(struct qbt *q, uint64_t deadline) { return run(q, deadline, 2); }
void qbt_delay(struct qbt *q, unsigned ms) {
    uint64_t end = now(q) + (uint64_t)ms * 1000000;
    while (now(q) < end) { q->ops.pump(q->ops.context); qbt_step(q); q->ops.wait_until(q->ops.context, end); }
}
void qbt_get_info(const struct qbt *q, struct qbt_info *out) { *out = q->info; }
void qbt_get_stats(const struct qbt *q, struct qbt_stats *out) { *out = q->stats; }
void qbt_linux_log(struct qbt *q, int level, const char *format, ...) {
    if (!q->ops.log) return;
    char text[160]; va_list ap; va_start(ap, format); vsnprintf(text, sizeof(text), format, ap); va_end(ap);
    q->ops.log(q->ops.context, level, text);
}
