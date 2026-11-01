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
    volatile uint32_t *writer_lock;
};

// Publish n contacts. Source arrays are the raw HID convention (xs 0..30480, ys 0..20320). Returns the number of
// contacts actually written (n clamped to slots).
static inline unsigned nvt_touch_publish_if(struct nvt_touch_publication *p, const int *xs, const int *ys,
                                         const int *ids, unsigned n, const unsigned *stop) {
    if (!p || !p->seq || !p->count || !p->writer_lock) return 0;
    if (n > p->slots) n = p->slots;
    if (n && (!xs || !ys || !ids || !p->xs || !p->ys || !p->ids)) return 0;
    // The physical and virtual producers share one writer lock. Drop a busy
    // frame rather than spin on a producer that may run on another CPU.
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(p->writer_lock, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    // Physical input owns the snapshot after takeover. Check under the writer lock
    // so an in-flight virtual callback cannot erase a newly published real finger.
    if (stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE) == 2) {
        __atomic_store_n(p->writer_lock, 0, __ATOMIC_RELEASE);
        return 0;
    }
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
    __atomic_store_n(p->writer_lock, 0, __ATOMIC_RELEASE);
    if (p->sink) p->sink(p->sink_context);
    return n;
}

static inline unsigned nvt_touch_publish(struct nvt_touch_publication *p, const int *xs, const int *ys,
                                         const int *ids, unsigned n) {
    return nvt_touch_publish_if(p, xs, ys, ids, n, NULL);
}

#endif
