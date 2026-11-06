// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared PIL session/metadata owner. An interrupted call owns its x6 session
// until it finishes; timeout quarantines PIL instead of starting another call.
#ifndef PICASSO_PAS_H
#define PICASSO_PAS_H
#include <stdint.h>
struct PicassoPasReply { uint32_t a0, a1, a2, a3; uint64_t a6; };
struct PicassoPasIo {
    void *context;
    PicassoPasReply (*call)(void *, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, bool);
    void (*delay)(void *, unsigned);
};
static inline int picasso_pas_execute(const PicassoPasIo *io, unsigned command,
    uint64_t address, uint64_t bytes, PicassoPasReply *reply) {
    if (!io || !io->call || !io->delay || !reply) return -22;
    if (command != 1 && command != 2 && command != 5 && command != 6) return -22;
    if ((command == 1 && (address != 0x8bc18000 || bytes)) ||
        (command == 2 && (address != 0x89200000 || bytes != 0x02800000)) ||
        (command >= 5 && (address || bytes))) return -22;
    const uint32_t arginfo = command == 1 ? 0x82 : command == 2 ? 3 : 1;
    const uint32_t function = 0x42000200 | command;
    unsigned interrupted = 0;
    for (unsigned busy = 0; busy < 68; ++busy) {
        *reply = io->call(io->context, function, arginfo, 1, address, bytes, 0, true);
        while (reply->a0 == 1) {
            if (++interrupted > 64) return -115; // Unknown completion; never cancel by overwriting metadata.
            io->delay(io->context, 1);
            *reply = io->call(io->context, 1, arginfo, 1, address, bytes, reply->a6, false);
        }
        if (reply->a0 != uint32_t(-12)) return reply->a0 ? -5 : reply->a1 ? -13 : 0;
        io->delay(io->context, 30);
    }
    return -16;
}
extern "C" int picasso_pas_acquire(void);
extern "C" void picasso_pas_release(bool uncertain);
extern "C" int picasso_pas_adsp(unsigned command, uint64_t address, uint64_t bytes);
#endif
