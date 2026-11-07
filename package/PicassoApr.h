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
// Picasso DTS: secondary RX SD0, AP master. Stock kona.c MI2S defaults:
// 48 kHz / S16_LE / two channels. Only cache the AFE port configuration;
// enabling clocks, DEVICE_START and amplifier/PCM setup are separate work.
inline uint32_t picasso_afe_config_word(unsigned n) {
    const uint32_t config[5]={1,0x00010010,0x00010001,48000,0};return config[n];
}
inline bool picasso_afe_stereo_unreported(const PicassoAprProbe &p) {
    // Picasso ADSP.VT.5.6.1: SET b05afa94..b05afa9c stores channels=2;
    // GET b05afdac unconditionally writes mono_stereo=0. Do not rewrite the
    // raw response or claim that stereo was read back. Other fields stay exact.
    if(p.fw.build[0]!=2 || p.fw.build[1]!=9 || p.fw.build[2] || p.fw.build[3] || p.afe.api!=7)return false;
    for(unsigned n=0;n<5;++n)
        if(p.afe.config[n]!=(n==2?0x00010000:picasso_afe_config_word(n)))return false;
    return true;
}
inline unsigned picasso_afe_request(unsigned phase,uint32_t (&w)[18]) {
    for(auto &v:w)v=0;
    if(phase!=1) {
        uint32_t query[14];picasso_afe_query(query);
        for(unsigned n=0;n<14;++n)w[n]=query[n];
        if(phase==2)w[3]=0x20520003;
        return 56;
    }
    w[0]=0x00440250;w[1]=0x504;w[2]=0x404;w[3]=0x20520002;w[4]=0x100ef;
    w[5]=0x00201002;w[9]=0x1020c;w[10]=0x1020d;w[11]=20;
    for(unsigned n=0;n<5;++n)w[12+n]=picasso_afe_config_word(n);
    return 68;
}
inline bool picasso_afe_domains_up(const PicassoQmi &q) {
    if(!q.bound || q.lost || q.round!=1)return false;
    for(const auto &d:q.domains)if(!d.completed || !d.stateValid || d.state!=0x1fffffff || d.error)return false;
    return true;
}
inline void picasso_afe_advance(PicassoAprProbe *p,const PicassoQmi &q) {
    auto &v=p->afe;
    if(!p->ready || !p->fw.valid)return;
    if(v.phase && !picasso_afe_domains_up(q)) {
        if(!v.error || v.pending || v.verified) {
            v.pending=v.valid=v.verified=v.readbackCompatible=false;v.error=-104;++v.events;
        }
        return;
    }
    bool configure=!v.phase && v.responded && v.error==-5 && v.status==2 &&
        v.opcode==0x10106 && v.payloadBytes==48 && v.raw[1]==0x1020c && v.raw[2]==0x1020d && !v.raw[3] &&
        picasso_afe_domains_up(q);
    bool verify=v.phase==1 && v.setAccepted && !v.error;
    if(!configure && !verify)return;
    ++v.phase;v.sent=v.pending=v.responded=v.valid=false;v.wait=v.status=v.opcode=0;
    v.payloadBytes=v.headerBytes=v.rejected=0;v.error=0;
    for(auto &word:v.raw)word=0;
    ++v.events;
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
// Stock q6afe_clk_set_params_v1 / afe_clk_set, secondary IBIT=0x102.
// Exercise a bounded 48k * 2 * 16 clock vote, then release it. No DEVICE_START,
// pins, amplifier or PCM; BASIC success confirms a vote, not a measured clock.
inline unsigned picasso_clock_request(unsigned phase,uint32_t (&w)[18]) {
    for(auto &v:w)v=0;
    w[0]=0x00440250;w[1]=0x504;w[2]=0x404;w[3]=0x20530001+phase;w[4]=0x100f3;
    w[5]=32;w[9]=0x1028f;w[10]=0x10290;w[11]=20;
    w[12]=1;w[13]=0x102;w[14]=1536000;w[15]=1;w[16]=phase?0:1;
    return 68;
}
inline void picasso_clock_stop(PicassoAprProbe *p,int error) {
    auto &c=p->clock;if(!c.attempted)return;
    c.pending=false;c.completed=false;c.error=error;++c.events;
    // Preserve uncertain until an actual disable ACK; SSR is not that ACK.
}
inline void picasso_clock_tick(PicassoAprProbe *p,const PicassoQmi &q) {
    auto &c=p->clock;
    if(!p->ready || !p->fw.valid)return;
    if(!c.attempted && p->afe.setAccepted && p->afe.valid && !p->afe.error &&
        (p->afe.verified || p->afe.readbackCompatible) && picasso_afe_domains_up(q)) {
        c.attempted=true;++c.events;
    }
    if(!c.attempted || c.completed)return;
    if(!c.phase && !picasso_afe_domains_up(q) && !c.error) {
        c.pending=false;c.error=-104;++c.events;
    }
    if(!c.phase && c.sent && !c.pending && (c.responded || c.error)) {
        c.enableError=c.error;c.phase=1;c.sent=c.pending=c.responded=false;
        c.wait=c.status=c.opcode=0;c.error=0;++c.events;
    }
    if(!c.error && (c.pending || !c.sent) && ++c.wait>(c.pending?100U:250U)) {
        c.pending=false;c.error=-60;++c.events;
    }
}
inline bool picasso_clock_due(const PicassoAprProbe &p) {
    const auto &c=p.clock;
    return p.ready && p.fw.valid && c.attempted && !c.sent && !c.error && !c.completed;
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
    if(p->clock.pending && p->receivedToken==p->clock.token) {
        auto &c=p->clock;const auto *b=data+headerBytes;
        if(type!=1 || p->source!=0x404 || p->destination!=0x504 || p->opcode!=0x110e8 ||
            size-headerBytes!=8 || wlan_sm32(b)!=0x100f3) { ++p->ignored;return; }
        c.status=wlan_sm32(b+4);c.opcode=p->opcode;c.pending=false;c.responded=true;c.error=c.status?-5:0;
        if(!c.phase)c.enableAccepted=!c.status;
        else if(!c.status) { c.disableAccepted=true;c.uncertain=false;c.completed=c.enableAccepted && !c.enableError; }
        ++c.events;return;
    }
    if(p->afe.pending && p->receivedToken==p->afe.token) {
        auto &v=p->afe;const auto *b=data+headerBytes;unsigned bytes=size-headerBytes;
        if(type>1 || p->source!=0x404 || p->destination!=0x504) { ++p->ignored;return; }
        if(v.phase==1) {
            if(p->opcode!=0x110e8 || type!=1 || bytes!=8 || wlan_sm32(b)!=0x100ef) { ++p->ignored;return; }
            v.status=wlan_sm32(b+4);v.opcode=p->opcode;v.pending=false;v.responded=true;
            v.setAccepted=!v.status;v.error=v.status?-5:0;++v.events;return;
        }
        if(p->opcode==0x110e8 && type==1 && bytes==8 && wlan_sm32(b)==0x100f0) {
            v.status=wlan_sm32(b+4);v.opcode=p->opcode;
            if(v.status) { v.pending=false;v.responded=true;v.error=-5;++v.events; }
            else ++p->ignored; // basic success is not configuration data
        } else if(p->opcode==0x10106) {
            if(bytes<4) { ++p->malformed;return; }
            uint32_t status=wlan_sm32(b);
            // Picasso firmware b01ed1f0..b01ed2e8 allocates param_hdr(12) +
            // requested payload_size(32) + status(4). The reported param_size
            // describes actual data, not the capacity of this 48-byte reply.
            // Stock q6afe also sends 32 here. Keep its wire contract and allow
            // the 12 unused capacity bytes; never decode them as parameters.
            if((bytes!=36 && bytes!=48) && (!status || (bytes!=4 && bytes!=16))) {
                ++p->malformed;return;
            }
            v.status=status;v.opcode=p->opcode;v.payloadBytes=bytes;v.headerBytes=headerBytes;
            for(unsigned i=0;i<12;++i)v.raw[i]=i<bytes/4?wlan_sm32(b+i*4):0;
            if(!status && (wlan_sm32(b+4)!=0x1020c || wlan_sm32(b+8)!=0x1020d ||
                wlan_sm32(b+12)!=20 || wlan_sm32(b+16)!=1 || (wlan_sm32(b+32)>>16))) {
                if(v.rejected<4) { ++v.rejected;++v.events; }
                ++p->malformed;return;
            }
            // A nonzero DSP status is an actual response, never a usable route.
            v.status=status;v.opcode=p->opcode;v.pending=false;v.responded=true;v.valid=!status;v.error=status?-5:0;
            if(!status)for(unsigned i=0;i<5;++i)v.config[i]=wlan_sm32(b+16+i*4);
            if(!status && v.phase==2) {
                v.verified=v.setAccepted;
                for(unsigned i=0;i<5;++i)if(v.config[i]!=picasso_afe_config_word(i))v.verified=false;
                v.stereoUnreported=v.setAccepted && picasso_afe_stereo_unreported(*p);
                v.readbackCompatible=v.verified || v.stereoUnreported;
                if(!v.readbackCompatible)v.error=-84;
            }
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
