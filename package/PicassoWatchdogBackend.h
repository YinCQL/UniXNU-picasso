/* SM7250 APSS watchdog shared with the early USB boot supervisor. */
#ifndef PICASSO_WATCHDOG_BACKEND_H
#define PICASSO_WATCHDOG_BACKEND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* owner must be non-NULL and remain alive for the rest of this boot.
 * A successful claim pets the already armed 15-second hardware watchdog and
 * permanently retires USB boot pets. A failed first claim never retains owner.
 * There is deliberately no release API.
 * Calls never wait: false includes lock contention and hardware mismatch.
 * A successful pet means RST was written with the expected control/timeout
 * readbacks; the write-only reset pulse itself has no acknowledgement.
 * Disable retains ownership. Enable restores the same 15-second hardware
 * configuration. Panic/reset permanently rejects ordinary operations. */
bool picasso_watchdog_backend_claim(const void *owner);
bool picasso_watchdog_backend_enable(const void *owner);
bool picasso_watchdog_backend_disable(const void *owner);
bool picasso_watchdog_backend_pet(const void *owner);

#ifdef __cplusplus
}
#endif
#endif
