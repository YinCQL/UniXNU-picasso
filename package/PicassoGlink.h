// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito GLINK SMEM transport and bounded APR CORE bring-up. No PCM endpoint.
#ifndef PICASSO_GLINK_H
#define PICASSO_GLINK_H
#include "PicassoSmp2p.h"
enum { PicassoGlinkChannels = 16, PicassoGlinkRemoteIntents = 64,
       PicassoGlinkLocalIntents = 20, PicassoGlinkPacket = 512 };
struct PicassoGlinkChannel {
    char name[32];
    uint16_t local, remote, priority;
    bool used, supported, openSent, openAck, ackSent, up, closing, closeAck;
    uint32_t steps, signals;
};
struct PicassoGlinkLocalIntent {
    uint32_t id, fill, left;
    bool advertised, active, done;
    uint8_t data[PicassoGlinkPacket];
};
struct PicassoGlinkRemoteIntent {
    uint32_t id, size;
    bool used, busy;
};
struct PicassoAprProbe {
    uint32_t token, attempts, ticks, wait, next, state, status, opcode, receivedToken;
    uint32_t received, ignored, malformed;
    bool pending, responded, ready, intentAsked, intentGranted;
    int error;
};
struct PicassoGlink {
    wlan_smem_item desc, tx, rx;
    uint32_t txHead, rxTail, steps, commands, retries;
    uint16_t pendingCommand, pendingChannel;
    uint32_t pendingData;
    bool prepared, sent, peerVersion, peerAck, ready, failed;
    int error;
    uint32_t channelEvents, deferredOpens, nextIntent, dataPackets;
    bool readNotify;
    PicassoGlinkChannel channel[PicassoGlinkChannels];
    PicassoGlinkLocalIntent localIntent[PicassoGlinkLocalIntents];
    PicassoGlinkRemoteIntent remoteIntent[PicassoGlinkRemoteIntents];
    PicassoAprProbe apr;
};
// Called under the same hardware allocator mutex as SMP2P, before PAS boot.
int picasso_glink_prepare(const PicassoSmemIo *, PicassoGlink *, PicassoSmemAudit *);
// Serialized ADSP worker, only while this boot's fresh slave-kernel is ready.
// At most eight RX commands and a bounded channel/TX pass per call. No waits.
// Kick IPCC client3/signal0 if set, including when returning TX backpressure.
int picasso_glink_service(const PicassoSmemIo *, PicassoGlink *, bool *kick);
// Invalidate all session readiness after fatal/SSR/shutdown; never reset SMEM.
void picasso_glink_stop(PicassoGlink *);
#endif
