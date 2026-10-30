/* Host transport model. The encoder, ring writers and drain are extracted
 * verbatim from the production patch; MMIO, clock and atomic APIs are models. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _Atomic
#define acquire 0
#define release 0
#define PU_PANIC_RING_SIZE (128u * 1024u)
#define PU_PANIC_RING_MASK (PU_PANIC_RING_SIZE - 1)
static struct { bool live, configured, tx_busy; uint32_t poll_lock; } ud;
static struct { char buffer[PU_PANIC_RING_SIZE]; uint64_t total; } pup_ring;
static uint64_t pup_pos;
static uint32_t pup_flush_lock;
static bool pu_panic_active;
static uint64_t now, clock_step = 1000;
static unsigned poll_calls, delay_calls;
static int scenario;
static FILE *wire;
static const unsigned char *input;
static unsigned input_size;
/* Production UD_TX is 4 KiB. Scenario 18 models a host that completes only
 * every 100 polls; this is a transport model, not a measured USB latency. */
static unsigned char in_flight[4096];
static unsigned in_flight_count;
static void picasso_usb_panic_stackshot(const void *, unsigned);
static bool os_atomic_cmpxchg(uint32_t *p, uint32_t old, uint32_t value, int order) {
    (void)order;
    if (*p != old) return false;
    *p = value;
    return true;
}
static void os_atomic_store(uint32_t *p, uint32_t value, int order) { (void)order; *p = value; }
static uint64_t mach_absolute_time(void) { uint64_t value = now; now += clock_step; return value; }
static void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) { *ns = ticks; }
static void picasso_usb_busy_wait(unsigned value) { assert(value == 1); ++delay_calls; }
static void ud_poll(void) {
    assert(pup_flush_lock == 1 && ud.poll_lock == 1);
    ++poll_calls;
    if (scenario == 9 || scenario == 10) return;
    if (scenario == 11 && poll_calls == 3) { ud.live = false; return; }
    if (scenario == 14) picasso_usb_panic_stackshot(input, input_size);
    if (scenario == 18 && poll_calls % 100 != 0) return;
    if (ud.tx_busy) {
        assert(fwrite(in_flight, 1, in_flight_count, wire) == in_flight_count);
        in_flight_count = 0;
        ud.tx_busy = false;
        return;
    }
    while (pup_pos < pup_ring.total && in_flight_count < sizeof(in_flight)) {
        in_flight[in_flight_count++] = (unsigned char)pup_ring.buffer[pup_pos++ & PU_PANIC_RING_MASK];
    }
    if (scenario == 16 || scenario == 18) { ud.tx_busy = in_flight_count != 0; return; }
    assert(fwrite(in_flight, 1, in_flight_count, wire) == in_flight_count);
    in_flight_count = 0;
}
static void picasso_usb_panic_write(const char *text, int size) {
    assert(pup_flush_lock == 0 && ud.poll_lock == 0);
    assert(fwrite(text, 1, (size_t)size, wire) == (size_t)size);
}
#include "stackshot-export.inc"

int main(int argc, char **argv) {
    assert(argc == 4);
    scenario = atoi(argv[1]);
    FILE *file = fopen(argv[2], "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long count = ftell(file); assert(count > 0 && (unsigned long)count <= PICASSO_STACKSHOT_MAX_BYTES);
    rewind(file);
    unsigned char *storage = malloc((size_t)count + 32); assert(storage);
    memset(storage, 0xa5, (size_t)count + 32);
    unsigned char *buffer = storage + 16;
    assert(fread(buffer, 1, (size_t)count, file) == (size_t)count); fclose(file);
    unsigned char *copy = malloc((size_t)count); assert(copy); memcpy(copy, buffer, (size_t)count);
    input = buffer; input_size = (unsigned)count;
    wire = fopen(argv[3], "wb"); assert(wire);
    pu_panic_active = ud.live = ud.configured = true;
    if (scenario == 1) pu_panic_active = false;
    if (scenario == 5) ud.live = false;
    if (scenario == 6) ud.configured = false;
    if (scenario == 7) pup_flush_lock = 1;
    if (scenario == 8) ud.poll_lock = 1;
    if (scenario == 9) clock_step = 1000000;
    if (scenario == 10) clock_step = 0;
    if (scenario == 12) now = UINT64_MAX - 10000;
    if (scenario == 13) {
        static const char prior[] = "PREEXISTING PANIC TEXT\r\n";
        pup_puts(prior, sizeof(prior) - 1);
        ud.tx_busy = true;
    }
    if (scenario == 17) clock_step = 2000000000;
    if (scenario == 2) picasso_usb_panic_stackshot(NULL, input_size);
    else if (scenario == 3) picasso_usb_panic_stackshot(buffer, 0);
    else if (scenario == 4) picasso_usb_panic_stackshot((const void *)(uintptr_t)1, UINT32_MAX);
    else picasso_usb_panic_stackshot(buffer, input_size);
    if (scenario == 15) picasso_usb_panic_stackshot(buffer, input_size);
    assert(pup_flush_lock == (scenario == 7 ? 1u : 0u));
    assert(ud.poll_lock == (scenario == 8 ? 1u : 0u));
    assert(poll_calls <= PICASSO_STACKSHOT_MAX_POLLS && poll_calls == delay_calls);
    if (scenario == 10) assert(poll_calls == PICASSO_STACKSHOT_MAX_POLLS);
    assert(memcmp(buffer, copy, (size_t)count) == 0);
    for (unsigned i = 0; i < 16; i++) assert(storage[i] == 0xa5 && storage[16 + count + i] == 0xa5);
    /* Model the existing final panic drain: no call back into the exporter. */
    assert(fwrite(in_flight, 1, in_flight_count, wire) == in_flight_count);
    assert(pup_ring.total - pup_pos < PU_PANIC_RING_SIZE);
    while (pup_pos < pup_ring.total) fputc(pup_ring.buffer[pup_pos++ & PU_PANIC_RING_MASK], wire);
    fclose(wire); free(copy); free(storage);
    printf("PASS case=%d polls=%u\n", scenario, poll_calls);
    return 0;
}
