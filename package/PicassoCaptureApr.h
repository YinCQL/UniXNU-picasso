// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent APR encoding from the target q6afe/q6adm/q6asm wire interfaces.
#ifndef PICASSO_CAPTURE_APR_H
#define PICASSO_CAPTURE_APR_H
#include "PicassoCapture.h"
inline uint32_t picasso_capture_acquires(unsigned phase) {
    switch(phase) {
    case PicassoCaptureVoteMacro:return PicassoCaptureMacroVote;
    case PicassoCaptureVoteCodec:return PicassoCaptureCodecVote;
    case PicassoCaptureCoreOn:return PicassoCaptureCoreClock;
    case PicassoCaptureNplOn:return PicassoCaptureNplClock;
    case PicassoCaptureConfigure:case PicassoCaptureStartPort:return PicassoCapturePort;
    case PicassoCaptureOpen:return PicassoCaptureStream;
    case PicassoCaptureMapBuffer:return PicassoCaptureMap;
    case PicassoCaptureOpenCopp:return PicassoCaptureCopp;
    default:return 0;
    }
}
inline unsigned picasso_capture_request(const PicassoCapture &v,uint32_t (&w)[18]) {
    for(auto &x:w)x=0;
    unsigned n=v.phase,bytes=20;
    w[1]=0x02010507;w[2]=0x02010407; // ASM session2 / stream1
    w[3]=picasso_capture_base(v)+1+n;
    if(n==PicassoCaptureVoteMacro || n==PicassoCaptureVoteCodec) {
        bytes=32;w[1]=0x504;w[2]=0x404;w[4]=0x100f4;
        w[5]=n==PicassoCaptureVoteMacro?3:4;
        w[6]=0x696d6370;w[7]=n==PicassoCaptureVoteMacro?0x006d2d63:0x00632d63; // pcmic-m / pcmic-c
    } else if(n==PicassoCaptureUnvoteCodec || n==PicassoCaptureUnvoteMacro) {
        unsigned index=n==PicassoCaptureUnvoteMacro?0:1;
        bytes=28;w[1]=0x504;w[2]=0x404;w[4]=0x100f6;w[5]=3+index;w[6]=v.hwHandle[index];
    } else if(n==PicassoCaptureCoreOn || n==PicassoCaptureNplOn || n==PicassoCaptureCoreOff || n==PicassoCaptureNplOff) {
        bytes=68;w[1]=0x504;w[2]=0x404;w[4]=0x100f3;
        w[5]=32;w[9]=0x1028f;w[10]=0x10290;w[11]=20;
        w[12]=1;w[13]=(n==PicassoCaptureCoreOn || n==PicassoCaptureCoreOff)?0x30c:0x30d;
        w[14]=19200000;w[15]=1;w[16]=n==PicassoCaptureCoreOn || n==PicassoCaptureNplOn;
    } else if(n==PicassoCaptureConfigure) {
        bytes=64;w[1]=0x504;w[2]=0x404;w[4]=0x100ef;
        w[5]=(28U<<16)|PicassoCapturePortId;w[9]=0x1020c;w[10]=0x102b8;w[11]=16;
        w[12]=1;w[13]=PicassoCaptureRate;w[14]=PicassoCaptureBits;w[15]=0x00010001;
    } else if(n==PicassoCaptureStartPort || n==PicassoCaptureStopPort) {
        bytes=24;w[1]=0x504;w[2]=0x404;w[4]=n==PicassoCaptureStartPort?0x100e5:0x100e6;
        w[5]=PicassoCapturePortId;
    } else if(n==PicassoCaptureOpen) {
        bytes=40;w[4]=0x10db4;w[5]=0x20000000; // low-latency TX, no metadata
        w[6]=0;w[7]=0x10c68;w[8]=0x10da5;w[9]=PicassoCaptureBits;
    } else if(n==PicassoCaptureFormat) {
        bytes=56;w[4]=0x10c10;w[5]=0x10da3;w[6]=28;w[7]=0;w[8]=20;
        w[9]=(PicassoCaptureBits<<16)|1;w[10]=PicassoCaptureRate;w[11]=1;w[12]=3; // center
    } else if(n==PicassoCaptureMapBuffer || n==PicassoCaptureUnmap) {
        bytes=n==PicassoCaptureMapBuffer?40:24;w[1]=0x507;w[2]=0x407;
        w[4]=n==PicassoCaptureMapBuffer?0x10d92:0x10d94;
        if(n==PicassoCaptureMapBuffer) {
            w[5]=0x10003;w[7]=uint32_t(v.dma.address);w[8]=uint32_t(v.dma.address>>32);w[9]=v.dma.bytes;
        } else w[5]=v.handle;
    } else if(n==PicassoCaptureOpenCopp) {
        bytes=48;w[1]=(PicassoCapturePortId<<16)|0x508;w[2]=(PicassoCapturePortId<<16)|0x408;w[4]=0x10326;
        w[5]=0x00022000;w[6]=0xffff0000U|PicassoCapturePortId;w[7]=0x10312;
        w[8]=(PicassoCaptureBits<<16)|1;w[9]=PicassoCaptureRate;w[10]=3;
    } else if(n==PicassoCaptureRoute) {
        bytes=36;w[1]=0x508;w[2]=0x408;w[4]=0x10325;
        w[5]=1;w[6]=1;w[7]=0x00010002;w[8]=v.copp; // TX matrix, session2, one COPP
    } else if(n==PicassoCaptureRun) {
        bytes=32;w[4]=0x10daa;
    } else if(picasso_capture_data_phase(n)) {
        bytes=40;w[4]=0x10dac;w[3]=picasso_capture_base(v)+0x1000+v.readSent;
        uint64_t address=v.dma.address+uint64_t(v.preparedSlot)*v.dma.slotBytes;
        w[5]=uint32_t(address);w[6]=uint32_t(address>>32);w[7]=v.handle;
        w[8]=v.dma.slotBytes;w[9]=v.readSent;
    } else if(n==PicassoCaptureClose)w[4]=0x10bcd;
    else if(n==PicassoCaptureCloseCopp) {
        w[1]=(PicassoCapturePortId<<16)|0x508;w[2]=(v.copp<<16)|0x408;w[4]=0x10327;
    } else return 0;
    w[0]=(bytes<<16)|0x250;return bytes;
}
inline void picasso_capture_sent(PicassoCapture &v) {
    if(picasso_capture_data_phase(v.phase)) {
        unsigned slot=v.preparedSlot;
        if(!v.prepared || slot>=2 || (v.inflight&(1U<<slot)) || (v.dma.busyMask&(1U<<slot))) {
            picasso_capture_fail(v,-22);return;
        }
        v.readSequence[slot]=v.readSent++;v.readWait[slot]=0;
        v.inflight|=1U<<slot;v.dma.busyMask|=1U<<slot;v.owned|=PicassoCaptureBuffer;
        v.prepared=false;
    } else {
        v.token=picasso_capture_base(v)+1+v.phase;v.sent=v.pending=true;
        v.owned|=picasso_capture_acquires(v.phase);v.wait=0;
    }
    ++v.events;
}
inline int picasso_capture_dma_check(PicassoCapture &v) {
    const auto &d=v.dma;
    if(!d.ready || !d.address || (d.address&4095) || !d.bytes || (d.bytes&4095) ||
       d.slotBytes<128 || d.slotBytes>16384 || (d.slotBytes&31) || d.slotBytes>d.bytes/2 ||
       d.address>0xfffffffffULL || d.bytes>0x1000000000ULL-d.address ||
       !d.check || !d.prepare || !d.complete)return -22;
    return v.dma.check(v.dma);
}
inline void picasso_capture_advance(PicassoCapture &v) {
    ++v.phase;v.wait=0;v.sent=v.pending=v.responded=v.prepared=false;++v.events;
}
// Elapsed ticks are 20 ms, matching the existing ADSP worker's deadline clock.
inline void picasso_capture_tick(PicassoCapture &v,bool transportReady,bool elapsed=true) {
    if(!v.enabled || v.done)return;
    if(!v.attempted) {
        if(!transportReady || !v.dma.ready) {
            if(elapsed && ++v.wait>250) {v.firstError=v.error=-60;v.done=true;++v.events;}
            return;
        }
        v.attempted=true;v.wait=0;
        if(!v.targetFrames)v.targetFrames=PicassoCaptureDefaultFrames;
        int e=!v.hardware || v.targetFrames>PicassoCaptureRate*10U ? -22:picasso_capture_dma_check(v);
        if(e) {v.firstError=v.error=e;v.dmaError=e;v.done=true;++v.events;return;}
        ++v.events;
    }
    if(!transportReady) {picasso_capture_stop(v,-104);return;}
    if(elapsed && v.phase>=PicassoCapturePrime && v.phase<=PicassoCaptureRead) {
        if(++v.streamTicks>1000)picasso_capture_fail(v,-60);
        for(unsigned slot=0;slot<2;++slot)
            if((v.inflight&(1U<<slot)) && ++v.readWait[slot]>100) {picasso_capture_fail(v,-60);break;}
    }
    if(v.responded && !v.pending)picasso_capture_advance(v);
    if(v.phase==PicassoCapturePrime && v.readSent>=2)picasso_capture_advance(v);
    // The callback may use a separate hardware worker. Poll it only here,
    // never in READ_DONE dispatch; receive can still retire owned buffers.
    if(v.healthPending || (v.phase==PicassoCaptureRead && v.readDone>=v.healthDone+8)) {
        if(!v.healthPending) {v.healthPending=true;v.healthWait=0;}
        int e=elapsed && ++v.healthWait>100 ? -60:v.hardware(v.context,3);
        if(e==-11)return;
        v.healthPending=false;v.healthWait=0;v.healthDone=v.readDone;++v.events;
        if(e) {
            if(v.phase<PicassoCaptureClose)picasso_capture_fail(v,e);
            else if(!v.cleanupError)v.cleanupError=e;
        }
    }
    if(v.phase==PicassoCaptureRead && v.bytesDone>=uint64_t(v.targetFrames)*2 && !v.inflight)
        picasso_capture_advance(v);
    // Skip only objects that were never acquired, or cannot be addressed safely.
    // Timed-out MAP/COPP OPEN keeps ownership visible even if no handle arrived.
    for(unsigned pass=0;pass<PicassoCaptureDone+1;++pass) {
        unsigned n=v.phase;
        bool skip=(n==PicassoCaptureClose && !(v.owned&PicassoCaptureStream)) ||
            (n==PicassoCaptureCloseCopp && (!(v.owned&PicassoCaptureCopp) || !v.coppValid)) ||
            (n==PicassoCaptureUnmap && (!(v.owned&PicassoCaptureMap) || !v.handle || (v.owned&(PicassoCaptureStream|PicassoCaptureBuffer)))) ||
            (n==PicassoCaptureStopPort && !(v.owned&PicassoCapturePort)) ||
            (n==PicassoCaptureHardwareStop && (!(v.owned&PicassoCaptureHardware) || (v.owned&(PicassoCapturePort|PicassoCaptureStream|PicassoCaptureCopp|PicassoCaptureBuffer)))) ||
            (n==PicassoCaptureNplOff && (!(v.owned&PicassoCaptureNplClock) || (v.owned&picasso_capture_dependencies()))) ||
            (n==PicassoCaptureCoreOff && (!(v.owned&PicassoCaptureCoreClock) || (v.owned&(picasso_capture_dependencies()|PicassoCaptureNplClock)))) ||
            (n==PicassoCaptureUnvoteCodec && (!(v.owned&PicassoCaptureCodecVote) || !v.hwHandle[1] || (v.owned&(picasso_capture_dependencies()|PicassoCaptureCoreClock|PicassoCaptureNplClock)))) ||
            (n==PicassoCaptureUnvoteMacro && (!(v.owned&PicassoCaptureMacroVote) || !v.hwHandle[0] || (v.owned&(picasso_capture_dependencies()|PicassoCaptureCoreClock|PicassoCaptureNplClock|PicassoCaptureCodecVote))));
        if(skip) {picasso_capture_advance(v);continue;}
        if(n==PicassoCapturePrepare || n==PicassoCaptureUnmute || n==PicassoCaptureHardwareStop) {
            if(elapsed && ++v.wait>250) {picasso_capture_fail(v,-60);continue;}
            unsigned action=n==PicassoCapturePrepare?0:n==PicassoCaptureUnmute?1:2;
            if(!action)v.owned|=PicassoCaptureHardware;
            int e=v.hardware(v.context,action);
            if(e==-11)return;
            if(e) {picasso_capture_fail(v,e);continue;}
            if(action==2)v.owned&=~PicassoCaptureHardware;
            v.accepted|=1U<<n;picasso_capture_advance(v);continue;
        }
        break;
    }
    if(v.phase==PicassoCaptureDone) {
        v.done=true;v.complete=!v.firstError && !v.cleanupError && !v.dmaError && !v.owned &&
            !v.dma.busyMask && !v.inflight && v.bytesDone>=uint64_t(v.targetFrames)*2;
        ++v.events;return;
    }
    if(!picasso_capture_data_phase(v.phase) && elapsed && ++v.wait>(v.pending?100U:250U))picasso_capture_fail(v,-60);
}
inline bool picasso_capture_due(PicassoCapture &v) {
    if(!v.enabled || !v.attempted || v.done || v.pending || v.sent || v.healthPending)return false;
    if(v.phase==PicassoCapturePrepare || v.phase==PicassoCaptureUnmute || v.phase==PicassoCaptureHardwareStop)return false;
    if(picasso_capture_data_phase(v.phase)) {
        if(v.phase==PicassoCapturePrime && v.readSent>=2)return false;
        if(v.bytesDone>=uint64_t(v.targetFrames)*2 || v.inflight==3)return false;
        if(v.readSent>=PicassoCaptureMaxReads) {picasso_capture_fail(v,-75);return false;}
        if(!v.prepared) {
            unsigned slot=(v.inflight&1)?1:0;
            if(v.dma.busyMask&(1U<<slot)) {picasso_capture_fail(v,-16);return false;}
            int e=picasso_capture_dma_check(v);
            if(!e)e=v.dma.prepare(v.dma,slot);
            if(e) {v.dmaError=e;picasso_capture_fail(v,e);return false;}
            v.preparedSlot=slot;v.prepared=true;
        }
    } else if(v.phase==PicassoCaptureMapBuffer) {
        int e=picasso_capture_dma_check(v);
        if(e) {v.dmaError=e;picasso_capture_fail(v,e);return false;}
    }
    uint32_t words[18];return picasso_capture_request(v,words)!=0;
}
inline bool picasso_capture_receive(PicassoCapture &v,const uint8_t *b,unsigned bytes,unsigned type,
                                   uint32_t source,uint32_t destination,uint32_t token,uint32_t opcode) {
    if(!v.attempted || v.done)return false;
    unsigned slot=2;
    for(unsigned i=0;i<2;++i)
        if((v.inflight&(1U<<i)) && token==picasso_capture_base(v)+0x1000+v.readSequence[i])slot=i;
    bool control=v.pending && token==v.token;
    if(slot==2 && !control)return false;
    v.replyPhase=v.phase;v.replyOpcode=opcode;v.replyType=type;v.replyBytes=bytes;
    v.replySource=source;v.replyDestination=destination;v.replyToken=token;v.replyReject=0;
    for(unsigned i=0;i<11;++i)v.replyRaw[i]=b && bytes>=4*(i+1)?picasso_capture_le32(b+i*4):0;
    ++v.events;
    auto reject=[&](unsigned why) {v.replyReject=why;++v.rejected;++v.events;return true;};
    if(!b && bytes)return reject(1);
    if(slot<2) {
        if(source!=0x02010407 || destination!=0x02010507)return reject(2);
        bool failure=opcode==0x110e8 && type==1 && bytes==8 && picasso_capture_le32(b)==0x10dac && picasso_capture_le32(b+4);
        uint32_t status=failure?picasso_capture_le32(b+4):0;
        if(!failure) {
            if(opcode!=0x10d9a || type!=0 || bytes!=44)return reject(3);
            uint64_t address=v.dma.address+uint64_t(slot)*v.dma.slotBytes;
            if(picasso_capture_le32(b+4)!=uint32_t(address) || picasso_capture_le32(b+8)!=uint32_t(address>>32) ||
               picasso_capture_le32(b+12)!=v.handle || picasso_capture_le32(b+40)!=v.readSequence[slot])return reject(4);
            status=picasso_capture_le32(b);
        }
        uint32_t size=failure?0:picasso_capture_le32(b+16),offset=failure?0:picasso_capture_le32(b+20);
        uint32_t flags=failure?0:picasso_capture_le32(b+32);
        if(!status && v.phase<PicassoCaptureClose && (!size || (size&1) || (offset&1) || offset>v.dma.slotBytes || size>v.dma.slotBytes-offset || (flags&0x7fffffffU))) {
            reject(5);picasso_capture_fail(v,-22);return true;
        }
        auto retire=[&]() {
            v.inflight&=~(1U<<slot);v.dma.busyMask&=~(1U<<slot);
            if(!v.inflight)v.owned&=~PicassoCaptureBuffer;
        };
        v.status=status;v.opcode=opcode;
        if(status) {
            retire();
            if(v.phase<PicassoCaptureClose)picasso_capture_fail(v,-5);
            else if(!v.cleanupError)v.cleanupError=-5;
            return true;
        }
        ++v.readDone;v.lastOffset=offset;v.lastBytes=size;v.lastFlags=flags;
        v.lastFrames=picasso_capture_le32(b+36);v.lastSequence=picasso_capture_le32(b+40);
        v.lastTimestamp=uint64_t(picasso_capture_le32(b+24))|(uint64_t(picasso_capture_le32(b+28))<<32);
        // Once cleanup starts, completion retires ownership but cannot publish data.
        if(v.phase>=PicassoCaptureClose) {retire();return true;}
        int e=picasso_capture_dma_check(v);
        if(!e)e=v.dma.complete(v.dma,slot,offset,size);
        retire();
        if(e) {v.dmaError=e;picasso_capture_fail(v,e);return true;}
        v.bytesDone+=size;return true;
    }
    uint32_t w[18];picasso_capture_request(v,w);
    if(source!=w[2] || destination!=w[1])return reject(2);
    bool basic=opcode==0x110e8 && type==1 && bytes==8 && picasso_capture_le32(b)==w[4];
    uint32_t status=0,n=v.phase;
    if((n==PicassoCaptureVoteMacro || n==PicassoCaptureVoteCodec) && opcode==0x100f5 && type==1 && bytes==4 && picasso_capture_le32(b))
        v.hwHandle[n==PicassoCaptureVoteMacro?0:1]=picasso_capture_le32(b);
    else if(n==PicassoCaptureMapBuffer && opcode==0x10d93 && type==1 && bytes==4 && picasso_capture_le32(b))v.handle=picasso_capture_le32(b);
    else if(n==PicassoCaptureOpenCopp && opcode==0x10329 && type==1 && bytes==8) {
        status=picasso_capture_le32(b);
        unsigned copp=picasso_capture_le32(b+4)&0xffff;
        if(!status && copp>=0xff) {reject(6);picasso_capture_fail(v,-22);return true;}
        if(!status) {v.copp=copp;v.coppValid=true;}
    } else if(basic) {
        status=picasso_capture_le32(b+4);
        if(!status && (n==PicassoCaptureMapBuffer || n==PicassoCaptureOpenCopp || n==PicassoCaptureVoteMacro || n==PicassoCaptureVoteCodec))return reject(7);
    } else return reject(3);
    v.status=status;v.opcode=opcode;v.pending=false;v.responded=true;
    if(status) {
        // An explicit failed MAP/COPP OPEN did not create an object. A timeout
        // or malformed success remains owned because its handle is unknown.
        if(n==PicassoCaptureVoteMacro)v.owned&=~PicassoCaptureMacroVote;
        if(n==PicassoCaptureVoteCodec)v.owned&=~PicassoCaptureCodecVote;
        if(n==PicassoCaptureMapBuffer)v.owned&=~PicassoCaptureMap;
        if(n==PicassoCaptureOpenCopp)v.owned&=~PicassoCaptureCopp;
        picasso_capture_fail(v,-5);return true;
    }
    v.accepted|=1U<<n;
    if(n==PicassoCaptureClose) {v.owned&=~(PicassoCaptureStream|PicassoCaptureBuffer);v.inflight=v.dma.busyMask=0;}
    else if(n==PicassoCaptureCloseCopp)v.owned&=~PicassoCaptureCopp;
    else if(n==PicassoCaptureUnmap)v.owned&=~PicassoCaptureMap;
    else if(n==PicassoCaptureStopPort)v.owned&=~PicassoCapturePort;
    else if(n==PicassoCaptureNplOff)v.owned&=~PicassoCaptureNplClock;
    else if(n==PicassoCaptureCoreOff)v.owned&=~PicassoCaptureCoreClock;
    else if(n==PicassoCaptureUnvoteCodec)v.owned&=~PicassoCaptureCodecVote;
    else if(n==PicassoCaptureUnvoteMacro)v.owned&=~PicassoCaptureMacroVote;
    return true;
}
inline bool picasso_capture_rearm(PicassoCapture &v) {
    bool cancelled=v.cancelled && v.firstError==-125 && !v.cleanupError && !v.dmaError;
    if(!v.done || (!v.complete && !cancelled) || v.owned || v.inflight || v.dma.busyMask ||
       picasso_capture_base(v)>=0x7ffe0000U)return false;
    auto dma=v.dma;auto hardware=v.hardware;void *context=v.context;
    uint32_t base=picasso_capture_base(v)+0x10000,frames=v.targetFrames,events=v.events+1;
    auto *bytes=reinterpret_cast<uint8_t *>(&v);for(unsigned i=0;i<sizeof v;++i)bytes[i]=0;
    v.dma=dma;v.hardware=hardware;v.context=context;v.tokenBase=base;
    v.targetFrames=frames;v.events=events;v.enabled=true;return true;
}
#endif
