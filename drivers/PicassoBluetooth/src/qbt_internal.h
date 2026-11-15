/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QBT_INTERNAL_H
#define QBT_INTERNAL_H
#include "qbt.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#else
#include <errno.h>
#include <string.h>
#include <stdio.h>
#endif
#define QBT_VERSION_EVENT 0xfeu /* First WCN399x reply: VSE or Command Complete. */
#define QBT_TX_CAPACITY 32768u
#define QBT_CHUNK 2048u
#define QBT_COMMAND_TIMEOUT 10000000000ULL
#define QBT_WAKE_RETRY 100000000ULL
#define QBT_TX_POLL 1000000ULL
#define QBT_SPEED_REPLY_MS 100u
#define QBT_SPEED_SETTLE_MS 5u
#define QBT_TX_IDLE 2000000000ULL
enum { QBT_ASLEEP, QBT_WAKING, QBT_AWAKE };
struct qbt_tx_frame { uint8_t type, header[4], have, need; unsigned body; };
struct qbt {
    struct qbt_platform ops;
    struct qbt_info info;
    struct qbt_stats stats;
    uint8_t tx[QBT_TX_CAPACITY];
    struct qbt_tx_frame tx_frame;
    unsigned tx_head, tx_length;
    bool service, ibs, rx_awake, clock_on;
    unsigned tx_state, wake_attempts;
    uint64_t retry_at, idle_at;
    uint8_t control[16]; unsigned control_length;
    uint8_t command[259]; unsigned command_length, command_offset;
    bool waiting, complete, speed_reply;
    uint16_t wait_opcode;
    uint8_t wait_event;
    uint8_t reply[258]; unsigned reply_length;
    uint8_t type, header[4]; unsigned header_length, header_need, body;
    uint8_t event[258]; unsigned event_length;
    bool drop;
    uint8_t forward[QBT_CHUNK]; unsigned forward_length, forward_offset;
};
int qbt_command(struct qbt *, uint16_t, const void *, unsigned, uint8_t,
                uint8_t *, size_t *);
int qbt_send_command(struct qbt *, uint16_t, const void *, unsigned);
int qbt_drain(struct qbt *, uint64_t);
void qbt_delay(struct qbt *, unsigned);
void qbt_linux_log(struct qbt *, int, const char *, ...);
#endif
