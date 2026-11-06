// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito GLINK SMEM control transport. No channel acknowledgement or APR/PCM yet.
#ifndef PICASSO_GLINK_H
#define PICASSO_GLINK_H
#include "PicassoSmp2p.h"
struct PicassoGlink {
    wlan_smem_item desc, tx, rx;
    uint32_t txHead, rxTail, steps, commands, retries;
    uint16_t pendingCommand, pendingChannel;
    uint32_t pendingData;
    bool prepared, sent, peerVersion, peerAck, ready, blocked, failed;
    int error;
};
// Called under the same hardware allocator mutex as SMP2P, before PAS boot.
int picasso_glink_prepare(const PicassoSmemIo *, PicassoGlink *, PicassoSmemAudit *);
// Serialized ADSP worker, only while this boot's fresh slave-kernel is ready.
// At most eight control commands per call. Kick IPCC client3/signal0 if set.
int picasso_glink_service(const PicassoSmemIo *, PicassoGlink *, bool *kick);
#endif
