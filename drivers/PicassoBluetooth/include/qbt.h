/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_H
#define QBT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qbt;
/* All protocol entry points run on one executor. IRQ and socket producers use
 * their own queues and wake it; callbacks never reenter a command operation. */
struct qbt_platform {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *, size_t);
    uint64_t (*now_ns)(void *);
    size_t (*uart_write)(void *, const uint8_t *, size_t);
    bool (*uart_drained)(void *);
    int (*uart_speed)(void *, uint32_t);
    int (*uart_flow)(void *, bool);
    /* clock(false) succeeds only with a real RX wake source armed. */
    int (*clock)(void *, bool);
    void (*pump)(void *);
    void (*wait_until)(void *, uint64_t);
    size_t (*receive)(void *, const uint8_t *, size_t);
    /* Returned firmware remains valid until firmware_put; NVM is copied. */
    int (*firmware_get)(void *, const char *, const uint8_t **, size_t *);
    void (*firmware_put)(void *, const uint8_t *, size_t);
    void (*log)(void *, int, const char *);
};
struct qbt_stats {
    uint64_t tx_bytes, rx_bytes, host_bytes, delivered_bytes;
    uint64_t commands, timeouts, malformed, unmatched_events;
    uint64_t wake_ind, wake_ack, sleep_ind, wake_retries, clock_sleeps;
    uint64_t backpressure, tx_rejected, resets;
    uint64_t tlv_sent, tlv_acks, tlv_final_status, hardware_errors, last_hardware_error;
};
struct qbt_info {
    unsigned state; /* 0 off, 1 initializing, 2 ready, 3 failed */
    int error;
    uint32_t baud, product, soc;
    uint16_t rom, patch;
    char firmware[64];
};
struct qbt *qbt_create(const struct qbt_platform *);
void qbt_destroy(struct qbt *);
/* Board executor only, once no old UART bytes can be delivered. */
void qbt_reset(struct qbt *);
/* Returns consumed input; the UART retains unconsumed bytes under backpressure. */
size_t qbt_receive(struct qbt *, const uint8_t *, size_t);
/* Atomic enqueue: 0 accepted, negative errno leaves ownership with the caller. */
int qbt_transmit(struct qbt *, const uint8_t *, size_t);
size_t qbt_transmit_capacity(const struct qbt *);
void qbt_step(struct qbt *);
uint64_t qbt_deadline(const struct qbt *);
void qbt_set_service(struct qbt *, bool);
void qbt_rx_wake(struct qbt *);
/* Abort command waiters promptly after a UART/DMA fault on the executor. */
void qbt_fail(struct qbt *, int error);
/* address uses display/network order; the Linux adapter converts to bdaddr_t. */
int qbt_boot(struct qbt *, uint32_t baud, const uint8_t address[6]);
int qbt_boot_wcn399x(struct qbt *, uint32_t baud, const uint8_t address[6]);
int qbt_shutdown(struct qbt *);
void qbt_get_info(const struct qbt *, struct qbt_info *);
void qbt_get_stats(const struct qbt *, struct qbt_stats *);
#ifdef __cplusplus
}
#endif
#endif
