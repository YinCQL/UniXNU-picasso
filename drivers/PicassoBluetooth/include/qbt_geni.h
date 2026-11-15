/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QBT_GENI_H
#define QBT_GENI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QBT_GENI_RX_COUNT 4u
#define QBT_GENI_RX_SIZE 2048u
#define QBT_GENI_TX_SIZE 4096u
/* Buffers are mapped for the device's actual DMA domain by the board adapter.
 * sync(for_device=true) publishes TX / relinquishes RX CPU ownership. */
struct qbt_dma_buffer { uint8_t *bytes; uint64_t address; size_t capacity; };
struct qbt_geni_ops {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*sync)(void *, const struct qbt_dma_buffer *, size_t, bool for_device, bool tx);
    uint64_t (*now_ns)(void *);
    /* GIC mask, usable in hard IRQ even when the serial clock is gated. */
    void (*irq_mask)(void *, bool);
    void (*wake_worker)(void *);
    size_t (*receive)(void *, const uint8_t *, size_t);
};
enum qbt_geni_state { QBT_GENI_OFF, QBT_GENI_RUN, QBT_GENI_CANCEL,
    QBT_GENI_ABORT, QBT_GENI_RESET, QBT_GENI_STOPPED, QBT_GENI_FAILED };
struct qbt_geni_stats {
    uint64_t interrupts, tx_bytes, rx_bytes, tx_dma, rx_dma;
    uint64_t rx_pauses, rx_backpressure, dma_errors, line_errors, cancel_timeouts;
};
struct qbt_geni {
    struct qbt_geni_ops ops;
    struct qbt_dma_buffer tx, rx[QBT_GENI_RX_COUNT];
    struct qbt_geni_stats stats;
    enum qbt_geni_state state;
    unsigned rx_head, rx_ready, rx_active;
    unsigned rx_length[QBT_GENI_RX_COUNT], rx_offset;
    unsigned tx_length;
    uint32_t baud, source_hz, oversampling;
    uint32_t fault_irq[4]; /* M, S, TX DMA and RX DMA status from a failed service turn. */
    uint64_t stop_deadline;
    int error;
    bool tx_busy, rx_busy, flow, irq_pending;
};
/* Engine must be idle, supplied with its UART microcode and functional clocks. */
int qbt_geni_prepare(struct qbt_geni *, const struct qbt_geni_ops *,
    const struct qbt_dma_buffer *, const struct qbt_dma_buffer [QBT_GENI_RX_COUNT]);
/* Source clock is already selected by the board; reject baud error above 1%. */
int qbt_geni_set_rate(struct qbt_geni *, uint32_t baud, uint32_t source_hz, unsigned oversampling);
int qbt_geni_start(struct qbt_geni *);
void qbt_geni_irq(struct qbt_geni *);
/* One bounded worker turn; true requests another turn without sleeping. */
bool qbt_geni_service(struct qbt_geni *);
size_t qbt_geni_write(struct qbt_geni *, const uint8_t *, size_t);
bool qbt_geni_drained(struct qbt_geni *);
void qbt_geni_set_flow(struct qbt_geni *, bool);
/* Cancel RX only after TX drains, then finish from IRQ or timed service turns.
 * Clock gating is allowed only at STOPPED with an external RX wake armed. */
int qbt_geni_quiesce(struct qbt_geni *);
int qbt_geni_resume(struct qbt_geni *);
uint64_t qbt_geni_deadline(const struct qbt_geni *);
#ifdef __cplusplus
}
#endif
#endif
