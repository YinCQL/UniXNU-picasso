// SPDX-License-Identifier: BSD-2-Clause-Patent
// QMP wire protocol: lito.dtsi and msm_qmp.c; all offsets are bytes.
#ifndef PICASSO_QMP_H
#define PICASSO_QMP_H
#include <stdint.h>

enum QmpState {
    QmpClosed = 0, QmpLinkWait, QmpChannelWait, QmpOpen, QmpSending, QmpClosing,
    QmpDescriptor = -22, QmpBusy = -16, QmpTimeout = -60,
    QmpDisconnected = -57, QmpReceivePending = -35, QmpTooLong = -7
};
struct QmpIo {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*kick)(void *);
};
struct PicassoQmp {
    QmpIo io;
    uint32_t ramBytes, tx, txBytes, rx, rxBytes;
    uint32_t descriptor[16];
    uint64_t deadline;
    int state;
    bool owned;
};
// One serialized owner per physical mailbox. Callers supply monotonic ticks
// and an absolute deadline. No allocation, sleeping, IRQ or unbounded loops.
int picasso_qmp_begin(PicassoQmp *, const QmpIo *, uint32_t ramBytes, uint64_t deadline);
int picasso_qmp_step(PicassoQmp *, uint64_t now);
int picasso_qmp_send(PicassoQmp *, const char *, uint64_t deadline);
int picasso_qmp_close(PicassoQmp *, uint64_t deadline);
extern "C" int picasso_aop_probe(void);
#endif
