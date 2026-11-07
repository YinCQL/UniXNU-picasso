// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included after PicassoApr.h helpers. Stock q6adm/q6asm wire protocol.
#ifndef PICASSO_PLAYBACK_APR_H
#define PICASSO_PLAYBACK_APR_H
inline unsigned picasso_play_acquires(unsigned phase) {
    switch(phase) {
    case PicassoPlayConfigure:return PicassoPlayPort;
    case PicassoPlayClockOn:return PicassoPlayClock;case PicassoPlayStart:return PicassoPlayPort;
    case PicassoPlayOpen:return PicassoPlayStream;case PicassoPlayMapBuffer:return PicassoPlayMap;
    case PicassoPlayOpenCopp:return PicassoPlayCopp;case PicassoPlayWrite:return PicassoPlayBuffer;default:return 0;
    }
}
inline unsigned picasso_play_request(const PicassoAprProbe &p,uint32_t (&w)[18]) {
    const auto &v=p.play;unsigned n=v.phase,bytes=20;
    for(auto &x:w)x=0;
    if(n==PicassoPlayConfigure)bytes=picasso_afe_request(1,w);
    else if(n==PicassoPlayClockOn || n==PicassoPlayClockOff)bytes=picasso_clock_request(n==PicassoPlayClockOff,w);
    else if(n==PicassoPlayStart || n==PicassoPlayStop)bytes=picasso_port_request(n==PicassoPlayStop,w);
    else if(n==PicassoPlayOpen || n==PicassoPlayFormat || n==PicassoPlayClose)bytes=picasso_stream_request(n==PicassoPlayOpen?0:n==PicassoPlayFormat?1:2,w);
    else if(n==PicassoPlayMapBuffer || n==PicassoPlayUnmap) {
        bytes=n==PicassoPlayMapBuffer?40:24;w[1]=0x507;w[2]=0x407;w[4]=n==PicassoPlayMapBuffer?0x10d92:0x10d94;
        if(n==PicassoPlayMapBuffer) {w[5]=0x10003;w[7]=uint32_t(p.pcm.address);w[8]=uint32_t(p.pcm.address>>32);w[9]=p.pcm.bytes;}
        else w[5]=v.handle;
    } else if(n==PicassoPlayOpenCopp) {
        bytes=48;w[1]=0x10020508;w[2]=0x10020408;w[4]=0x10326;
        w[5]=0x00012000;w[6]=0xffff1002;w[7]=0x10312; // LL playback, NULL COPP
        w[8]=0x00100002;w[9]=48000;w[10]=0x201;
    } else if(n==PicassoPlayRoute) {
        bytes=36;w[1]=0x508;w[2]=0x408;w[4]=0x10325;
        w[5]=0;w[6]=1;w[7]=0x10001;w[8]=v.copp;
    } else if(n==PicassoPlayRun) {
        bytes=32;w[1]=0x01010507;w[2]=0x01010407;w[4]=0x10daa; // immediate RUN, timestamp zero
    } else if(n==PicassoPlayWrite) {
        bytes=52;w[1]=0x01010507;w[2]=0x01010407;w[4]=0x10dab;
        w[5]=uint32_t(p.pcm.address);w[6]=uint32_t(p.pcm.address>>32);w[7]=v.handle;
        w[8]=4096;w[9]=1; // 1024 stereo S16 silence frames, no valid timestamp
    } else if(n==PicassoPlayCloseCopp) {
        w[1]=0x10020508;w[2]=(v.copp<<16)|0x408;w[4]=0x10327;
    } else return 0;
    w[0]=(bytes<<16)|0x250;w[3]=0x20570001+n;return bytes;
}
inline void picasso_play_sent(PicassoPlayback &v) {
    v.sent=v.pending=true;v.token=0x20570001+v.phase;v.wait=0;
    v.owned|=picasso_play_acquires(v.phase);++v.events;
}
inline void picasso_play_tick(PicassoAprProbe &p,const PicassoQmi &q) {
    auto &v=p.play;
    if(v.enabled && !v.attempted && p.pcm.done && p.pcm.complete && !p.pcm.uncertain &&
        p.tfa.digitalApplied && p.tfa.digitalRestored && !p.tfa.digitalError && !p.tfa.digitalRestoreError && !p.tfa.digitalDirty &&
        picasso_afe_domains_up(q)) {
        v.attempted=true;++v.events;
        v.pins.context=p.pins.context;v.pins.read=p.pins.read;v.pins.write=p.pins.write;
        for(unsigned i=0;i<p.fw.count;++i)if(p.fw.services[i][0]==8)v.api=p.fw.services[i][1];
        if(!v.api) {v.error=v.firstError=-95;v.done=true;return;}
    }
    if(!v.attempted || v.done)return;
    if(v.phase<PicassoPlayClose && !picasso_afe_domains_up(q) && !v.error) {
        v.pending=false;v.error=-104;++v.events;
    }
    if(!v.pending && (v.responded || v.error)) {
        if(v.error) {
            if(v.phase<PicassoPlayClose && !v.firstError)v.firstError=v.error;
            if(v.phase>=PicassoPlayClose && !v.cleanupError)v.cleanupError=v.error;
        }
        v.phase=v.phase<PicassoPlayClose && v.error?PicassoPlayClose:v.phase+1;
        v.sent=v.responded=false;v.error=0;v.wait=v.status=v.opcode=0;++v.events;
    }
    // Stock SET marks port+0x20 configured (b05a340c); STOP clears it
    // (b01ecdc4). Every START needs a fresh SET, as in q6afe __afe_port_start.
    // Close clients before removing their DMA mapping or physical clock.
    // A timed-out MAP/COPP OPEN without a handle stays explicitly owned.
    for(;;) {
        if(v.phase==PicassoPlayClose && !(v.owned&PicassoPlayStream))++v.phase;
        else if(v.phase==PicassoPlayCloseCopp && (!(v.owned&PicassoPlayCopp) || !(v.accepted&(1U<<PicassoPlayOpenCopp))))++v.phase;
        else if(v.phase==PicassoPlayUnmap && (!(v.owned&PicassoPlayMap) || !v.handle || (v.owned&PicassoPlayBuffer)))++v.phase;
        else if(v.phase==PicassoPlayStop && !(v.owned&PicassoPlayPort))++v.phase;
        // STOP must acknowledge before removing a possibly running port's clock.
        else if(v.phase==PicassoPlayClockOff && (!(v.owned&PicassoPlayClock) || (v.owned&PicassoPlayPort)))++v.phase;
        else break;
    }
    // COPP zero is valid; use the accepted bit, not a truthy ID.
    if(v.phase==PicassoPlayDone) {
        picasso_audio_pins_restore(v.pins);
        v.done=true;v.complete=v.consumed && v.accepted==((1U<<PicassoPlayDone)-1) && !v.owned &&
            !v.firstError && !v.cleanupError && !v.dmaError && v.pins.restored && !v.pins.dirty;
        ++v.events;return;
    }
    if(!v.sent && !v.error && v.phase==PicassoPlayStart) {
        v.error=picasso_audio_pins_activate(v.pins);
        if(v.error) {++v.events;return;}
    }
    if(!v.error && ++v.wait>(v.pending?100U:250U)) {v.pending=false;v.error=-60;++v.events;}
}
inline bool picasso_play_due(PicassoAprProbe &p) {
    auto &v=p.play;
    if(!p.ready || !p.fw.valid || !v.attempted || v.done || v.sent || v.error)return false;
    if(v.phase==PicassoPlayWrite && !v.bufferReady) {
        v.dmaError=p.pcm.silence?p.pcm.silence(p.pcm):-22;
        if(v.dmaError) {v.error=v.dmaError;++v.events;return false;}
        v.bufferReady=true;
    }
    if(v.phase==PicassoPlayMapBuffer || v.phase==PicassoPlayWrite) {
        v.dmaError=p.pcm.check?p.pcm.check(p.pcm):-22;
        if(v.dmaError) {v.error=v.dmaError;++v.events;return false;}
    }
    return true;
}
inline bool picasso_play_receive(PicassoAprProbe &p,const uint8_t *b,unsigned bytes,unsigned type) {
    auto &v=p.play;
    if(!v.pending || p.receivedToken!=v.token)return false;
    uint32_t w[18];picasso_play_request(p,w);
    if(p.source!=w[2] || p.destination!=w[1]) {++p.ignored;return true;}
    unsigned n=v.phase;
    bool basic=p.opcode==0x110e8 && type==1 && bytes==8 && wlan_sm32(b)==w[4];
    uint32_t status=0;int health=0;
    if(n==PicassoPlayMapBuffer && p.opcode==0x10d93 && type==1 && bytes==4 && wlan_sm32(b))v.handle=wlan_sm32(b);
    else if(n==PicassoPlayOpenCopp && p.opcode==0x10329 && type==1 && bytes==8 && !(wlan_sm32(b+4)>>16)) {
        status=wlan_sm32(b);
        if(!status && wlan_sm32(b+4)>=0xff) {++p.ignored;return true;}
        if(!status)v.copp=wlan_sm32(b+4);
    } else if(n==PicassoPlayWrite && p.opcode==0x10d99 && type==0 && bytes==16 &&
        wlan_sm32(b)==uint32_t(p.pcm.address) && wlan_sm32(b+4)==uint32_t(p.pcm.address>>32) && wlan_sm32(b+8)==v.handle) {
        status=wlan_sm32(b+12);v.writeAddressLow=wlan_sm32(b);v.writeAddressHigh=wlan_sm32(b+4);
        v.writeHandle=wlan_sm32(b+8);v.writeStatus=status;v.owned&=~PicassoPlayBuffer;
        health=p.pcm.check?p.pcm.check(p.pcm):-22;v.consumed=!status && !health;
    } else if(basic) {
        status=wlan_sm32(b+4);
        if(!status && (n==PicassoPlayMapBuffer || n==PicassoPlayOpenCopp || n==PicassoPlayWrite)) {++p.ignored;return true;}
    } else {++p.ignored;return true;}
    v.pending=false;v.responded=true;v.status=v.statuses[n]=status;v.opcode=p.opcode;v.error=status?-5:0;
    // Configuration/START can leave port state behind even on failure.
    // Stock START status9 means either already running OR not configured;
    // neither proves an idle port. Keep it owned until a real STOP ACK.
    if(status)v.owned&=~(picasso_play_acquires(n)&~PicassoPlayPort);
    else {
        v.accepted|=1U<<n;
        if(n==PicassoPlayClose)v.owned&=~(PicassoPlayStream|PicassoPlayBuffer);
        if(n==PicassoPlayCloseCopp)v.owned&=~PicassoPlayCopp;
        if(n==PicassoPlayUnmap) {v.owned&=~PicassoPlayMap;health=p.pcm.check?p.pcm.check(p.pcm):-22;}
        if(n==PicassoPlayStop)v.owned&=~PicassoPlayPort;
        if(n==PicassoPlayClockOff)v.owned&=~PicassoPlayClock;
    }
    if(health) {if(!v.dmaError)v.dmaError=health;if(!v.error)v.error=health;}
    ++v.events;return true;
}
#endif
