// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_SMEM_LOCK_H
#define PICASSO_SMEM_LOCK_H
#include <stdint.h>
struct PicassoSmemLockIo {
    void *context;
    uint32_t (*read)(void *);
    void (*write)(void *, uint32_t);
    void (*barrier)(void *);
};
// APPS has hardware ID 1. A local atomic gate is also mandatory: all APPS
// CPUs would otherwise see ID 1 and believe that they own the same mutex.
static inline int picasso_smem_trylock(const PicassoSmemLockIo *io) {
    uint32_t owner = io->read(io->context);
    if (owner) return owner == 1 ? -16 : -11;
    io->write(io->context, 1);
    io->barrier(io->context);
    return io->read(io->context) == 1 ? 0 : -11;
}
static inline int picasso_smem_unlock(const PicassoSmemLockIo *io) {
    io->barrier(io->context);
    if (io->read(io->context) != 1) return -16;
    io->write(io->context, 0);
    io->barrier(io->context);
    // Another processor may acquire immediately; never write 0 a second time.
    return 0;
}
#endif
