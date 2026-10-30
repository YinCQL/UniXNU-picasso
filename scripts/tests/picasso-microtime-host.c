/* The actual XNU functions are included without edits. Only the clock input
 * and output types are host stand-ins; this does not test timer IRQ delivery. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t clock_sec_t;
typedef uint32_t clock_usec_t;
#define USEC_PER_SEC UINT64_C(1000000)
static uint32_t rtclock_sec_divisor;
static uint64_t host_ticks;
static uint64_t mach_absolute_time(void) { return host_ticks; }
#include "microtime.inc"

struct vector { uint32_t rate; uint64_t ticks, seconds; uint32_t usecs; };
static const struct vector vectors[] = {
#include "vectors.inc"
};

int main(void)
{
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct vector *v = &vectors[i];
        struct seconds { uint64_t before, value, after; } sec = {123, 0, 456};
        struct useconds { uint32_t before, value, after; } usec = {789, 0, 321};
        rtclock_sec_divisor = v->rate;
        host_ticks = v->ticks;
        absolutetime_to_microtime(v->ticks, &sec.value, &usec.value);
        assert(sec.value == v->seconds && usec.value == v->usecs);
        assert(usec.value < 1000000);
        assert(sec.before == 123 && sec.after == 456 && usec.before == 789 && usec.after == 321);
        sec.value = 0; usec.value = 0;
        clock_get_system_microtime(&sec.value, &usec.value);
        assert(sec.value == v->seconds && usec.value == v->usecs);
        assert(sec.before == 123 && sec.after == 456 && usec.before == 789 && usec.after == 321);
    }
    // Exhaust every remainder of the real counter rate, through a second
    // rollover. Normalization and monotonicity are behavioral invariants.
    rtclock_sec_divisor = 19200000;
    uint64_t last_seconds = 0;
    uint32_t last_usecs = 0;
    for (uint64_t tick = 0; tick <= 19200000; ++tick) {
        uint64_t sec; uint32_t usec;
        absolutetime_to_microtime(tick, &sec, &usec);
        assert(usec < 1000000);
        assert(sec > last_seconds || (sec == last_seconds && usec >= last_usecs));
        last_seconds = sec; last_usecs = usec;
    }
    assert(last_seconds == 1 && last_usecs == 0);
    puts("PASS microtime oracle, output widths, normalization and second rollover");
    return 0;
}
