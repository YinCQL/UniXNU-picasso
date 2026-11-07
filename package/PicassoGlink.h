// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito GLINK SMEM, APR/QRTR and bounded FastRPC listener. No PCM endpoint.
#ifndef PICASSO_GLINK_H
#define PICASSO_GLINK_H
#include "PicassoSmp2p.h"
#include "PicassoQrtr.h"
#include "PicassoFastrpc.h"
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
    bool delivered;
    uint32_t capacity;
    uint8_t *data;
};
struct PicassoGlinkRemoteIntent {
    // The peer owns the ID namespace: zero is valid; used tracks occupancy.
    uint32_t id, size;
    bool used, busy;
};
struct PicassoAprProbe {
    uint32_t token, attempts, ticks, wait, next, state, status, opcode, receivedToken;
    uint32_t header, source, destination, bytes;
    uint32_t received, ignored, malformed;
    bool pending, responded, ready, intentAsked, intentGranted;
    int error;
    struct {
        uint32_t token, wait, status, build[4], count, services[25][3], events;
        bool sent, pending, valid;
        int error;
    } fw;
    struct {
        uint32_t token,wait,status,opcode,events,api,config[5];
        uint32_t payloadBytes,headerBytes,raw[12],rejected;
        uint32_t phase; // 0 initial GET, 1 SET configuration, 2 verify GET
        bool setAccepted,verified;
        bool sent,pending,responded,valid;
        int error;
    } afe;
};
struct PicassoGlink {
    // Contains pointers into its own static storage: prepare in final location,
    // never copy a prepared session or place it on a kernel stack.
    wlan_smem_item desc, tx, rx;
    uint32_t txHead, rxTail, steps, commands, retries;
    uint16_t pendingCommand, pendingChannel;
    uint32_t pendingData;
    bool prepared, sent, peerVersion, peerAck, ready, failed;
    int error;
    uint32_t channelEvents, deferredOpens, nextIntent, dataPackets;
    uint32_t intentFrames, intentOffers, intentSkipped, intentReplaced;
    uint32_t intentProgress, intentCount, intentTail, intentChannel;
    uint32_t lastIntentId, lastIntentSize, intentFault;
    uint32_t remoteDone, remoteReuse, localReuse, lastDoneId, lastDoneChannel;
    // Preserve bounded wire evidence for the next unhandled or invalid frame.
    uint32_t faultBytes, faultWords[8];
    bool readNotify;
    PicassoGlinkChannel channel[PicassoGlinkChannels];
    PicassoGlinkLocalIntent localIntent[PicassoGlinkLocalIntents];
    PicassoGlinkRemoteIntent remoteIntent[PicassoGlinkRemoteIntents];
    PicassoGlinkLocalIntent qrtrLocal[10];
    PicassoGlinkRemoteIntent qrtrRemote[PicassoGlinkRemoteIntents];
    uint8_t aprStorage[PicassoGlinkLocalIntents][PicassoGlinkPacket];
    // lito.dtsi: IPCRTR 5*0x800, 3*0x2000, 2*0x4400. Static worker storage.
    uint8_t qrtrStorage[5*0x800 + 3*0x2000 + 2*0x4400];
    // Stock lito.dtsi: fastrpcglink-apps-dsp, 64 receive intents of 0x64 bytes.
    PicassoGlinkLocalIntent fastrpcLocal[64];
    PicassoGlinkRemoteIntent fastrpcRemote[PicassoGlinkRemoteIntents];
    uint8_t fastrpcStorage[64][100];
    PicassoFastrpc fastrpc;
    PicassoAprProbe apr;
    PicassoQrtr qrtr;
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
