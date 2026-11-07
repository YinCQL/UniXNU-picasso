// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire constants from Picasso stock techpack/audio/{ipc/apr.c,include/ipc/apr.h,
// dsp/q6core.c,include/dsp/q6core.h,dsp/q6afe.c,include/dsp/apr_audio-v2.h}.
// Independent encoding, no GPR/AudioReach.
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
// Stock q6afe_get_params_v2: in-band, no memory mapping, GET only. Secondary
// MI2S RX is the Picasso TFA98xx playback interface. No clocks/port start here.
inline void picasso_afe_query(uint32_t (&w)[14]) {
    for(auto &v:w)v=0;
    w[0]=0x00380250;w[1]=0x504;w[2]=0x404;w[3]=0x20520001;w[4]=0x100f0;
    w[5]=(32U<<16)|0x1002; // param_hdr_v1 (12) + i2s_cfg (20)
    w[9]=0x1020c;w[10]=0x1020d; // audio device interface / I2S config
    w[11]=0x1020c;w[12]=0x1020d;w[13]=20;
}
inline bool picasso_afe_due(PicassoAprProbe *p) {
    auto &v=p->afe;
    if(!p->ready || !p->fw.valid || v.sent || v.error)return false;
    for(unsigned n=0;n<p->fw.count;++n)if(p->fw.services[n][0]==4) {
        v.api=p->fw.services[n][1];
        if(!v.api) { v.error=-95;++v.events;return false; }
        return true;
    }
    v.error=-2;++v.events;return false;
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
    if(p->afe.pending && p->receivedToken==p->afe.token) {
        auto &v=p->afe;const auto *b=data+headerBytes;unsigned bytes=size-headerBytes;
        if(type>1 || p->source!=0x404 || p->destination!=0x504) { ++p->ignored;return; }
        if(p->opcode==0x110e8 && type==1 && bytes==8 && wlan_sm32(b)==0x100f0) {
            v.status=wlan_sm32(b+4);v.opcode=p->opcode;
            if(v.status) { v.pending=false;v.responded=true;v.error=-5;++v.events; }
            else ++p->ignored; // basic success is not configuration data
        } else if(p->opcode==0x10106) {
            if(bytes<4) { ++p->malformed;return; }
            uint32_t status=wlan_sm32(b);
            if(!status && (bytes!=36 || wlan_sm32(b+4)!=0x1020c || wlan_sm32(b+8)!=0x1020d ||
                wlan_sm32(b+12)!=20 || wlan_sm32(b+16)!=1 || (wlan_sm32(b+32)>>16))) {
                ++p->malformed;return;
            }
            // A nonzero DSP status is an actual response, never a usable route.
            if(status && bytes!=4 && bytes!=16 && bytes!=36) { ++p->malformed;return; }
            v.status=status;v.opcode=p->opcode;v.pending=false;v.responded=true;v.valid=!status;v.error=status?-5:0;
            if(!status)for(unsigned i=0;i<5;++i)v.config[i]=wlan_sm32(b+16+i*4);
            ++v.events;
        } else ++p->ignored;
        return;
    }
    bool version=p->fw.pending && p->receivedToken==p->fw.token;
    if ((!version && (!p->pending || p->receivedToken != p->token)) || type>1 ||
        wlan_sm32(data + 4) != 0x403 || wlan_sm32(data + 8) != 0x503) {
        ++p->ignored; return;
    }
    if(version) {
        auto &v=p->fw;const uint8_t *payload=data+headerBytes;unsigned bytes=size-headerBytes;
        if(p->opcode==0x1292d) {
            if(bytes<20) { ++p->malformed;return; }
            unsigned count=wlan_sm32(payload+16);
            if(count>25 || bytes!=20+count*12) { ++p->malformed;return; }
            for(unsigned n=0;n<count;++n) for(unsigned j=0;j<n;++j)
                if(wlan_sm32(payload+20+n*12)==wlan_sm32(payload+20+j*12)) { ++p->malformed;return; }
            for(unsigned n=0;n<4;++n)v.build[n]=wlan_sm32(payload+n*4);
            for(unsigned n=0;n<count;++n)for(unsigned j=0;j<3;++j)v.services[n][j]=wlan_sm32(payload+20+n*12+j*4);
            v.count=count;v.pending=false;v.valid=true;++v.events;
        } else if(type==1 && p->opcode==0x110e8 && bytes==8 && wlan_sm32(payload)==0x1292c) {
            v.status=wlan_sm32(payload+4);
            if(v.status) { v.error=-5;v.pending=false;++v.events; }else ++p->ignored;
        } else ++p->ignored;
    } else if (p->opcode == 0x1290d && size == headerBytes + 4) {
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
