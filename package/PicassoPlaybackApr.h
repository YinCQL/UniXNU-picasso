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
        uint64_t address=p.pcm.address+(picasso_play_queued(v)?(v.writeSent&1)*PicassoPcmSlotBytes:0);
        w[5]=uint32_t(address);w[6]=uint32_t(address>>32);w[7]=v.handle;
        w[8]=picasso_play_bytes(v);
        w[9]=picasso_play_queued(v)?picasso_play_base(v)+0x1001+v.writeSent:1; // stock seq_id after buf_size; timestamps/flags zero
    } else if(n==PicassoPlayDrain) {
        w[1]=0x01010507;w[2]=0x01010407;w[4]=0x10bdb; // EOS, wait for RENDERED_EOS
    } else if(n==PicassoPlayCloseCopp) {
        w[1]=0x10020508;w[2]=(v.copp<<16)|0x408;w[4]=0x10327;
    } else return 0;
    w[0]=(bytes<<16)|0x250;w[3]=n==PicassoPlayWrite && picasso_play_queued(v)?picasso_play_base(v)+0x1001+v.writeSent:picasso_play_base(v)+1+n;return bytes;
}
inline void picasso_play_sent(PicassoAprProbe &p) {
    auto &v=p.play;
    v.sent=v.pending=true;v.token=picasso_play_base(v)+1+v.phase;v.wait=0;
    v.owned|=picasso_play_acquires(v.phase);
    if(v.phase==PicassoPlayWrite) {
        unsigned slot=picasso_play_queued(v)?v.writeSent&1:0;p.pcm.busyMask|=1U<<slot;
        if(picasso_play_queued(v)) {
            v.token=picasso_play_base(v)+0x1001+v.writeSent;v.writeSequence[slot]=v.writeSent;
            v.writeWait[slot]=0;v.inflight|=1U<<slot;
            v.writeBytes[slot]=picasso_play_bytes(v);v.bytesSent+=v.writeBytes[slot];++v.writeSent;
            v.sent=v.bufferReady=false; // another free slot may be queued
        }
    }
    ++v.events;
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
    // A new source session can wait for domain readiness before any command.
    // Bound that wait as well; no resources have been acquired at this point.
    if(v.enabled && v.source && !v.attempted && !v.done && ++v.wait>250) {
        v.error=v.firstError=-60;v.done=true;++v.events;
    }
    if(!v.attempted || v.done)return;
    if(v.phase<PicassoPlayClose && !picasso_afe_domains_up(q) && !v.error) {
        v.pending=false;v.error=-104;++v.events;
    }
    if(v.phase==PicassoPlayWrite && picasso_play_queued(v) && !v.error) {
        // Per-buffer deadlines and a total bound; reply traffic cannot extend it.
        ++v.streamTicks;
        if(!v.source && v.streamTicks>500)v.error=-60;
        if(v.source && !v.inflight && !v.bufferReady && !v.sourceEnded && ++v.starveTicks>100)v.error=-60;
        // A prepared chunk can still wait forever for a GLINK intent or FIFO.
        // Bound publication independently of producer starvation/completions.
        if(v.source && !v.inflight && !v.sourceEnded && ++v.wait>250)v.error=-60;
        if(v.source && v.sourceEnded && !v.inflight && !v.error) {
            v.consumed=v.writeSent && v.writeDone==v.writeSent;
            if(v.consumed) {v.accepted|=1U<<PicassoPlayWrite;v.responded=true;}
            else v.error=-22;
        }
        for(unsigned i=0;i<2;++i)if((v.inflight&(1U<<i)) && ++v.writeWait[i]>100)v.error=-60;
        if(v.error) {v.pending=false;++v.events;}
    }
    // A local fill/health/transport refusal may occur with the other slot
    // still owned. Abort scheduling immediately; CLOSE must retire that slot.
    if(v.phase==PicassoPlayWrite && v.error)v.pending=false;
    if(!v.pending && (v.responded || v.error)) {
        if(v.error) {
            if(v.phase<PicassoPlayClose && !v.firstError)v.firstError=v.error;
            if(v.phase>=PicassoPlayClose && !v.cleanupError)v.cleanupError=v.error;
        }
        v.phase=v.phase<PicassoPlayClose && v.error?PicassoPlayClose:v.phase+1;
        v.sent=v.responded=false;v.error=0;v.wait=v.status=v.opcode=0;++v.events;
    }
    if(v.phase>=PicassoPlayClose && v.amplifier && !v.ampFinished) {
        int e=v.amplifier(v,2);v.ampFinished=true;
        if(e && !v.cleanupError)v.cleanupError=e;++v.events;
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
        v.done=true;v.complete=v.consumed && v.drained && (!v.amplifier || v.amp.stopped) && v.accepted==((1U<<PicassoPlayDone)-1) && !v.owned &&
            !v.firstError && !v.cleanupError && !v.dmaError && v.pins.restored && !v.pins.dirty;
        ++v.events;return;
    }
    if(!v.sent && !v.error && v.phase==PicassoPlayStart) {
        v.error=picasso_audio_pins_activate(v.pins);
        if(v.error) {++v.events;return;}
    }
    if(v.phase==PicassoPlayWrite && picasso_play_queued(v))return;
    if(!v.error && ++v.wait>(v.pending?100U:250U)) {v.pending=false;v.error=-60;++v.events;}
}
inline bool picasso_play_due(PicassoAprProbe &p) {
    auto &v=p.play;
    if(!p.ready || !p.fw.valid || !v.attempted || v.done || v.sent || v.error)return false;
    if(v.amplifier && v.phase==PicassoPlayOpen && !v.ampPrepared) {
        v.ampPrepared=true;v.error=v.amplifier(v,0);++v.events;if(v.error)return false;
    }
    if(v.amplifier && v.phase==PicassoPlayWrite && !v.ampEnabled) {
        v.error=v.amplifier(v,1);++v.events;if(v.error)return false;v.ampEnabled=true;v.tone=!v.source;
    }
    if(v.phase==PicassoPlayWrite && picasso_play_queued(v)) {
        if(v.sourceEnded || (!v.source && v.writeSent>=PicassoPcmToneChunks) || (v.inflight&(1U<<(v.writeSent&1))))return false;
        if(v.writeSent>=0xeffe) {v.error=-75;++v.events;return false;} // never reuse a token

        if(v.amplifier && v.writeDone>=v.healthDone+8) {
            v.healthDone=v.writeDone;v.error=v.amplifier(v,3);++v.events;if(v.error)return false;
        }
    }
    if(v.amplifier && v.phase==PicassoPlayDrain) {
        v.error=v.amplifier(v,3);if(v.error) {++v.events;return false;}
    }
    if(v.phase==PicassoPlayWrite && !v.bufferReady) {
        if(v.source) {
            unsigned bytes=0;int result=v.source(v,p.pcm,v.writeSent&1,&bytes);
            if(result==-11)return false;
            if(result==-125) {v.error=result;++v.events;return false;}
            if(result==1 && !bytes) {v.sourceEnded=true;++v.events;return false;}
            v.dmaError=result?result:(!bytes || bytes>PicassoPcmSlotBytes || (bytes&3))?-22:0;
            if(!v.dmaError) {v.nextBytes=bytes;v.starveTicks=0;}
        } else v.dmaError=v.tone?(p.pcm.tone?p.pcm.tone(p.pcm,v.writeSent&1,v.writeSent):-22):(p.pcm.silence?p.pcm.silence(p.pcm):-22);
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
    bool streaming=v.phase==PicassoPlayWrite && picasso_play_queued(v);unsigned slot=2;
    if(streaming)for(unsigned i=0;i<2;++i)
        if((v.inflight&(1U<<i)) && p.receivedToken==picasso_play_base(v)+0x1001+v.writeSequence[i])slot=i;
    if(!v.pending || (streaming?slot==2:p.receivedToken!=v.token))return false;
    v.replyPhase=v.phase;v.replyOpcode=p.opcode;v.replyType=type;v.replyBytes=bytes;
    v.replySource=p.source;v.replyDestination=p.destination;v.replyReject=0;
    for(unsigned i=0;i<4;++i)v.replyRaw[i]=bytes>=4*(i+1)?wlan_sm32(b+4*i):0;
    ++v.events;
    auto reject=[&](unsigned reason) {v.replyReject=reason;++v.rejected;++p.ignored;return true;};
    uint32_t w[18];picasso_play_request(p,w);
    if(p.source!=w[2] || p.destination!=w[1])return reject(1);
    unsigned n=v.phase;
    bool basic=p.opcode==0x110e8 && type==1 && bytes==8 && wlan_sm32(b)==w[4];
    uint32_t status=0;int health=0,protocol=0;
    if(streaming) {
        uint64_t address=p.pcm.address+slot*PicassoPcmSlotBytes;
        if(p.opcode==0x10d99 && type==0 && bytes==16 && wlan_sm32(b)==uint32_t(address) &&
            wlan_sm32(b+4)==uint32_t(address>>32) && wlan_sm32(b+8)==v.handle) {
            status=wlan_sm32(b+12);v.writeAddressLow=wlan_sm32(b);v.writeAddressHigh=wlan_sm32(b+4);
            v.writeHandle=wlan_sm32(b+8);v.writeStatus=status;
        } else if(basic && wlan_sm32(b+4))status=wlan_sm32(b+4);
        else return reject(basic?4:2);
        v.inflight&=~(1U<<slot);p.pcm.busyMask&=~(1U<<slot);
        if(!v.inflight)v.owned&=~PicassoPlayBuffer;
        health=p.pcm.check?p.pcm.check(p.pcm):-22;
        v.status=v.statuses[n]=status;v.opcode=p.opcode;
        if(status || health) {
            v.error=status?-5:health;v.dmaError=health;v.pending=false;
        } else {
            ++v.writeDone;v.bytesDone+=v.writeBytes[slot];
            v.consumed=(v.source?v.sourceEnded && v.writeSent && v.writeDone==v.writeSent:v.writeDone==PicassoPcmToneChunks && v.writeSent==PicassoPcmToneChunks) && !v.inflight;
            v.pending=!!v.inflight;v.responded=v.consumed;
            if(v.consumed)v.accepted|=1U<<n;
        }
        ++v.events;return true;
    }
    if(n==PicassoPlayMapBuffer && p.opcode==0x10d93 && type==1 && bytes==4 && wlan_sm32(b))v.handle=wlan_sm32(b);
    else if(n==PicassoPlayOpenCopp && p.opcode==0x10329 && type==1 && bytes==8) {
        status=wlan_sm32(b);
        // Stock q6adm reads u16 copp_id and ignores the reserved halfword.
        // Picasso success path b067cffc..d010 writes status and u16 ID only;
        // the following stack halfword is not initialized. Preserve it in
        // replyRaw for diagnostics, never use it as part of the route ID.
        unsigned id=wlan_sm32(b+4)&0xffff;
        if(!status && id>=0xff) {protocol=-22;v.replyReject=3;++v.rejected;}
        else if(!status)v.copp=id;
    } else if(n==PicassoPlayWrite && p.opcode==0x10d99 && type==0 && bytes==16 &&
        wlan_sm32(b)==uint32_t(p.pcm.address) && wlan_sm32(b+4)==uint32_t(p.pcm.address>>32) && wlan_sm32(b+8)==v.handle) {
        status=wlan_sm32(b+12);v.writeAddressLow=wlan_sm32(b);v.writeAddressHigh=wlan_sm32(b+4);
        v.writeHandle=wlan_sm32(b+8);v.writeStatus=status;v.owned&=~PicassoPlayBuffer;p.pcm.busyMask=0;
        health=p.pcm.check?p.pcm.check(p.pcm):-22;v.consumed=!status && !health;
    } else if(n==PicassoPlayDrain && p.opcode==0x10c1c && type==0 && bytes==0) {
        v.drained=true;
    } else if(basic) {
        status=wlan_sm32(b+4);
        if(!status && (n==PicassoPlayMapBuffer || n==PicassoPlayOpenCopp || n==PicassoPlayWrite || n==PicassoPlayDrain))return reject(4);
    } else return reject(2);
    v.pending=false;v.responded=true;v.status=v.statuses[n]=status;v.opcode=p.opcode;v.error=status?-5:protocol;
    // Configuration/START can leave port state behind even on failure.
    // Stock START status9 means either already running OR not configured;
    // neither proves an idle port. Keep it owned until a real STOP ACK.
    if(status) {v.owned&=~(picasso_play_acquires(n)&~PicassoPlayPort);if(n==PicassoPlayWrite)p.pcm.busyMask=0;}
    else if(!protocol) {
        v.accepted|=1U<<n;
        if(n==PicassoPlayClose) {v.owned&=~(PicassoPlayStream|PicassoPlayBuffer);v.inflight=p.pcm.busyMask=0;}
        if(n==PicassoPlayCloseCopp)v.owned&=~PicassoPlayCopp;
        if(n==PicassoPlayUnmap) {v.owned&=~PicassoPlayMap;health=p.pcm.check?p.pcm.check(p.pcm):-22;}
        if(n==PicassoPlayStop)v.owned&=~PicassoPlayPort;
        if(n==PicassoPlayClockOff)v.owned&=~PicassoPlayClock;
    }
    if(health) {if(!v.dmaError)v.dmaError=health;if(!v.error)v.error=health;}
    ++v.events;return true;
}
inline bool picasso_play_rearm(PicassoAprProbe &p) {
    auto &v=p.play;
    bool cancelled=v.firstError==-125 && !v.cleanupError && !v.dmaError;
    bool fresh=!v.enabled && !v.attempted && !v.done && !v.error && !v.firstError &&
        !v.cleanupError && !v.dmaError && !v.sent && !v.pending && !v.ampPrepared && !v.ampEnabled;
    if((!fresh && (!v.done || (!v.complete && !cancelled))) || v.owned || v.inflight || p.pcm.busyMask || v.pins.dirty ||
        (!fresh && v.amplifier && !v.amp.stopped) || picasso_play_base(v)>=0x7ffe0000)return false;
    auto amp=v.amplifier;auto source=v.source;auto pins=v.pins;
    unsigned base=picasso_play_base(v)+0x10000,events=v.events+1;
    // Byte clearing avoids placing a complete playback state on the kernel stack.
    auto *bytes=reinterpret_cast<uint8_t *>(&v);for(unsigned i=0;i<sizeof v;++i)bytes[i]=0;
    v.amplifier=amp;v.source=source;v.enabled=true;v.tokenBase=base;v.events=events;
    v.pins.context=pins.context;v.pins.read=pins.read;v.pins.write=pins.write;return true;
}
#endif
