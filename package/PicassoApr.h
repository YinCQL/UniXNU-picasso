// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire constants from Picasso stock techpack/audio/{ipc/apr.c,include/ipc/apr.h,
// dsp/q6core.c,include/dsp/q6core.h}. Independent encoding, no GPR/AudioReach.
#ifndef PICASSO_APR_H
#define PICASSO_APR_H
#include "PicassoGlink.h"
inline void picasso_apr_query(uint32_t token, uint32_t (&words)[5]) {
    words[0] = 0x00140250; // 20 bytes, sequential command, 5-word header, v0
    words[1] = 0x00000503; // APPS/CORE, port 0
    words[2] = 0x00000403; // ADSP/CORE, port 0
    words[3] = token;
    words[4] = 0x0001290c; // AVCS_CMD_ADSP_EVENT_GET_STATE
}
inline void picasso_apr_receive(PicassoAprProbe *p, const uint8_t *data, uint32_t size) {
    ++p->received;
    p->bytes=size;
    p->header=size>=4?wlan_sm32(data):0;
    p->source=size>=8?wlan_sm32(data+4):0;
    p->destination=size>=12?wlan_sm32(data+8):0;
    if (size < 20) { ++p->malformed; return; }
    uint32_t header = wlan_sm32(data);
    uint32_t headerBytes = ((header >> 4) & 15) * 4;
    p->opcode = wlan_sm32(data + 16); p->receivedToken = wlan_sm32(data + 12);
    // Stock apr.c accepts v0/v1 and locates the payload after hdr_size. Build186
    // returns v1, eight header words and an EVENT (0x00240081), not CMD_RSP.
    if ((header & 15) > 1 || headerBytes < 20 || headerBytes > size || (header >> 16) != size) {
        ++p->malformed; return;
    }
    uint32_t type=(header>>8)&3;
    if (!p->pending || p->receivedToken != p->token || type>1 ||
        wlan_sm32(data + 4) != 0x403 || wlan_sm32(data + 8) != 0x503) {
        ++p->ignored; return;
    }
    if (p->opcode == 0x1290d && size == headerBytes + 4) {
        p->state = wlan_sm32(data + headerBytes); p->responded = true;
        p->ready = p->state != 0; p->pending = false; p->next = p->ticks + 5;
    } else if (type==1 && p->opcode == 0x110e8 && size == headerBytes + 8 &&
               wlan_sm32(data + headerBytes) == 0x1290c) {
        p->status = wlan_sm32(data + headerBytes + 4);
        // A basic success is not the requested state response.
        if (p->status) { p->error = -5; p->pending = false; }
        else ++p->ignored;
    } else ++p->ignored;
}
#endif
