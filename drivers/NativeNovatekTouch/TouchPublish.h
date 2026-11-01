// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch-producer half of the HID publication contract. NativeHidInput.cpp reads the snapshot under a seqlock:
// the sequence is odd while a frame is being written, the reader retries when it changes. The sink registered by the
// digitizer is called after the sequence closes, so the HID driver submits the report from the publishing thread.
#ifndef NATIVE_NOVATEK_TOUCH_PUBLISH_H
#define NATIVE_NOVATEK_TOUCH_PUBLISH_H
#include <stdint.h>
#include <stddef.h>

struct nvt_touch_publication {
    volatile uint32_t *seq;
    volatile uint32_t *count;
    volatile int *xs, *ys, *ids;
    unsigned slots;
    void (*sink)(void *);
    void *sink_context;
};

// Publish n contacts. Source arrays are the raw HID convention (xs 0..30480, ys 0..20320). Returns the number of
// contacts actually written (n clamped to slots).
static inline unsigned nvt_touch_publish(struct nvt_touch_publication *p, const int *xs, const int *ys,
                                         const int *ids, unsigned n) {
    if (!p || !p->seq || !p->count) return 0;
    if (n > p->slots) n = p->slots;
    const uint32_t seq = *p->seq;
    *p->seq = seq + 1;                      // odd: the snapshot is being written
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *p->count = n;
    for (unsigned i = 0; i < n; ++i) {
        p->xs[i] = xs[i];
        p->ys[i] = ys[i];
        p->ids[i] = ids[i];
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *p->seq = seq + 2;                      // even: the snapshot is complete
    if (p->sink) p->sink(p->sink_context);
    return n;
}

#endif
