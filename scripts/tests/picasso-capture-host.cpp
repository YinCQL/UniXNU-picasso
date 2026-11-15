// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PicassoCaptureApr.h"
struct Fixture {
    PicassoCapture v{};
    uint8_t data[8192]{};
    unsigned prepared{},completed{},hwPrepare{},hwUnmute{},hwStop{},hwHealth{},healthPendingReturns{},offCommands{};
    uint64_t delivered{};
    int checkError{},prepareError{},completeError{},hardwareError{},stopError{},healthError{};
    static Fixture &get(PicassoCaptureDma &d) {return *static_cast<Fixture *>(d.context);}
    static int check(PicassoCaptureDma &d) {return get(d).checkError;}
    static int prepare(PicassoCaptureDma &d,unsigned slot) {
        auto &f=get(d);assert(slot<2 && !(d.busyMask&(1U<<slot)));++f.prepared;
        memset(f.data+4096*slot,0xa5,4096);return f.prepareError;
    }
    static int complete(PicassoCaptureDma &d,unsigned slot,unsigned offset,unsigned bytes) {
        auto &f=get(d);assert(slot<2 && (d.busyMask&(1U<<slot)));
        assert(!(bytes&1) && bytes && offset<=4096 && bytes<=4096-offset);
        for(unsigned i=0;i<bytes;++i)assert(f.data[slot*4096+offset+i]==uint8_t(i*13+7));
        ++f.completed;if(f.completeError)return f.completeError;f.delivered+=bytes;return 0;
    }
    static int hardware(void *context,unsigned action) {
        auto &f=*static_cast<Fixture *>(context);auto &v=f.v;
        assert(v.hwHandle[0] && v.hwHandle[1]);
        assert((v.accepted&(1U<<PicassoCaptureCoreOn)) && (v.accepted&(1U<<PicassoCaptureNplOn)));
        if(action==0) {++f.hwPrepare;return f.hardwareError;}
        if(action==1) {++f.hwUnmute;return 0;}
        if(action==3) {
            assert(v.healthPending);++f.hwHealth;
            if(f.healthPendingReturns) {--f.healthPendingReturns;return -11;}
            return f.healthError;
        }
        assert(action==2 && !(v.owned&(PicassoCapturePort|PicassoCaptureStream|PicassoCaptureCopp|PicassoCaptureBuffer)));
        ++f.hwStop;return f.stopError;
    }
    Fixture() {
        v.dma.context=this;v.dma.check=check;v.dma.prepare=prepare;v.dma.complete=complete;
        v.dma.address=0x180008000ULL;v.dma.bytes=32768;v.dma.slotBytes=4096;v.dma.ready=v.dma.retained=true;
        v.context=this;v.hardware=hardware;v.enabled=true;v.targetFrames=96000;
    }
    void control(uint32_t dspStatus=0) {
        uint32_t w[18];unsigned bytes=picasso_capture_request(v,w);assert(bytes && v.pending);
        uint32_t b[2]={w[4],dspStatus},opcode=0x110e8,n=v.phase;
        if(!dspStatus && (n==PicassoCaptureVoteMacro || n==PicassoCaptureVoteCodec)) {opcode=0x100f5;b[0]=0x80+n;bytes=4;}
        else if(!dspStatus && n==PicassoCaptureMapBuffer) {opcode=0x10d93;b[0]=0x7788;bytes=4;}
        else if(!dspStatus && n==PicassoCaptureOpenCopp) {opcode=0x10329;b[0]=0;b[1]=0xbeef0000;bytes=8;}
        else bytes=8;
        if(n==PicassoCaptureNplOff || n==PicassoCaptureCoreOff || n==PicassoCaptureUnvoteCodec || n==PicassoCaptureUnvoteMacro)++offCommands;
        assert(picasso_capture_receive(v,reinterpret_cast<uint8_t *>(b),bytes,1,w[2],w[1],v.token,opcode));
    }
    void payload(unsigned slot,uint32_t (&b)[11],unsigned bytes=3840,unsigned offset=128) {
        assert(slot<2 && (v.inflight&(1U<<slot)));memset(b,0,sizeof b);
        uint64_t address=v.dma.address+slot*4096;
        b[1]=uint32_t(address);b[2]=uint32_t(address>>32);b[3]=v.handle;b[4]=bytes;b[5]=offset;
        b[6]=uint32_t(v.bytesDone*1000000/96000);b[8]=0x80000000;b[9]=1;b[10]=v.readSequence[slot];
        if(offset<=4096 && bytes<=4096-offset)for(unsigned i=0;i<bytes;++i)data[slot*4096+offset+i]=uint8_t(i*13+7);
    }
    bool deliver(unsigned slot,uint32_t (&b)[11],unsigned bytes=44,uint32_t token=0,uint32_t source=0x02010407) {
        return picasso_capture_receive(v,reinterpret_cast<uint8_t *>(b),bytes,0,source,0x02010507,
            token?token:picasso_capture_read_token(v,v.readSequence[slot]),0x10d9a);
    }
    void step(bool dataReplies=true) {
        picasso_capture_tick(v,true);
        if(picasso_capture_due(v)) {picasso_capture_sent(v);if(v.pending)control();}
        if(dataReplies && v.phase>=PicassoCaptureRead && v.phase<=PicassoCaptureClose) {
            for(unsigned slot=2;slot>0;--slot)if(v.inflight&(1U<<(slot-1))) {
                uint32_t b[11];payload(slot-1,b);assert(deliver(slot-1,b));
            }
        }
    }
    void until(unsigned phase) {
        for(unsigned i=0;i<600 && !v.done;++i) {
            picasso_capture_tick(v,true);
            if(v.phase==phase)break;
            if(picasso_capture_due(v)) {picasso_capture_sent(v);if(v.pending)control();}
        }
        assert(v.phase==phase && !v.done);
    }
    void finish() {for(unsigned i=0;i<1500 && !v.done;++i)step();assert(v.done);}
};
static void wire() {
    Fixture f;uint32_t w[18];auto &v=f.v;
    struct Vector {unsigned phase,bytes,opcode,source,destination;};
    const Vector vectors[]={
        {PicassoCaptureVoteMacro,32,0x100f4,0x504,0x404},{PicassoCaptureVoteCodec,32,0x100f4,0x504,0x404},
        {PicassoCaptureCoreOn,68,0x100f3,0x504,0x404},{PicassoCaptureNplOn,68,0x100f3,0x504,0x404},
        {PicassoCaptureConfigure,64,0x100ef,0x504,0x404},{PicassoCaptureStartPort,24,0x100e5,0x504,0x404},
        {PicassoCaptureOpen,40,0x10db4,0x02010507,0x02010407},{PicassoCaptureFormat,56,0x10c10,0x02010507,0x02010407},
        {PicassoCaptureMapBuffer,40,0x10d92,0x507,0x407},{PicassoCaptureOpenCopp,48,0x10326,0xb0370508,0xb0370408},
        {PicassoCaptureRoute,36,0x10325,0x508,0x408},{PicassoCaptureRun,32,0x10daa,0x02010507,0x02010407},
        {PicassoCapturePrime,40,0x10dac,0x02010507,0x02010407},{PicassoCaptureClose,20,0x10bcd,0x02010507,0x02010407},
        {PicassoCaptureUnvoteCodec,28,0x100f6,0x504,0x404}};
    for(const auto &a:vectors) {v.phase=a.phase;assert(picasso_capture_request(v,w)==a.bytes);assert(w[0]==(a.bytes<<16|0x250));assert(w[1]==a.source && w[2]==a.destination && w[4]==a.opcode);}
    v.phase=PicassoCaptureConfigure;picasso_capture_request(v,w);
    assert(w[5]==0x001cb037 && w[9]==0x1020c && w[10]==0x102b8 && w[11]==16);
    assert(w[12]==1 && w[13]==48000 && w[14]==16 && w[15]==0x00010001);
    v.phase=PicassoCaptureFormat;picasso_capture_request(v,w);
    assert(w[5]==0x10da3 && w[6]==28 && w[7]==0 && w[8]==20 && w[9]==0x00100001 && w[10]==48000 && w[11]==1 && w[12]==3);
    v.phase=PicassoCaptureOpenCopp;picasso_capture_request(v,w);assert(w[5]==0x00022000 && w[6]==0xffffb037 && w[7]==0x10312);
    v.phase=PicassoCaptureRoute;picasso_capture_request(v,w);assert(w[5]==1 && w[7]==0x00010002);
    v.phase=PicassoCaptureMapBuffer;picasso_capture_request(v,w);assert(w[5]==0x10003 && w[7]==0x80008000 && w[8]==1 && w[9]==32768);
}
static void lifecycle() {
    Fixture f;f.finish();auto &v=f.v;
    assert(v.complete && !v.owned && !v.inflight && !v.dma.busyMask && v.dma.retained);
    assert(v.bytesDone==f.delivered && f.delivered>=192000 && f.completed==v.readDone);
    assert(v.coppValid && v.copp==0 && f.hwPrepare==1 && f.hwUnmute==1 && f.hwStop==1 && f.offCommands==4 && f.hwHealth>=5);
    uint32_t oldBase=picasso_capture_base(v);assert(picasso_capture_rearm(v));assert(picasso_capture_base(v)==oldBase+0x10000);
    f.finish();assert(v.complete && f.hwPrepare==2 && f.hwStop==2);
}
static void malformed() {
    Fixture f;f.until(PicassoCaptureRead);auto &v=f.v;assert(v.inflight==3);
    uint32_t b[11];f.payload(0,b,128,64);uint32_t token=picasso_capture_base(v)+0x1000+v.readSequence[0];
    assert(!f.deliver(0,b,44,token+123));assert(v.inflight==3 && !f.completed);
    assert(f.deliver(0,b,44,token,0x01010407));assert(v.replyReject==2 && v.inflight==3);
    assert(f.deliver(0,b,40));assert(v.replyReject==3 && !f.completed);
    ++b[3];assert(f.deliver(0,b));assert(v.replyReject==4 && v.inflight==3);--b[3];
    ++b[10];assert(f.deliver(0,b));assert(v.replyReject==4);--b[10];
    uint32_t ok[2]={0x10dac,0};assert(picasso_capture_receive(v,reinterpret_cast<uint8_t *>(ok),8,1,0x02010407,0x02010507,token,0x110e8));assert(v.inflight==3);
    assert(f.deliver(0,b));assert(f.completed==1 && f.delivered==128 && v.inflight==2 && v.dma.busyMask==2);
    assert(!picasso_capture_receive(v,reinterpret_cast<uint8_t *>(b),44,0,0x02010407,0x02010507,token,0x10d9a));
    assert(f.completed==1);f.payload(1,b,128,0xfffffffeU);assert(f.deliver(1,b));
    assert(v.firstError==-22 && v.phase==PicassoCaptureClose && v.dma.busyMask==2 && f.completed==1);
    f.finish();assert(!v.complete && !v.owned && v.firstError==-22);
    Fixture m;m.until(PicassoCaptureRead);m.payload(0,b);b[8]=0x40000000;assert(m.deliver(0,b));assert(m.v.firstError==-22 && !m.completed);
    Fixture o;o.until(PicassoCaptureRead);o.payload(0,b);b[4]=3;assert(o.deliver(0,b));assert(o.v.firstError==-22 && !o.completed);
}
static void controlQualification() {
    Fixture f;picasso_capture_tick(f.v,true);assert(picasso_capture_due(f.v));picasso_capture_sent(f.v);
    uint32_t w[18];picasso_capture_request(f.v,w);uint32_t basic[2]={0x100f4,0};
    assert(picasso_capture_receive(f.v,reinterpret_cast<uint8_t *>(basic),8,1,0x404,0x504,f.v.token,0x110e8));
    assert(f.v.pending && !f.v.hwHandle[0] && !f.hwPrepare);
    f.control();f.finish();assert(f.v.complete);
}
static void failures() {
    Fixture map;map.until(PicassoCaptureMapBuffer);assert(picasso_capture_due(map.v));picasso_capture_sent(map.v);
    for(unsigned i=0;i<102 && map.v.phase==PicassoCaptureMapBuffer;++i)picasso_capture_tick(map.v,true);
    map.finish();assert(map.v.firstError==-60 && !map.v.complete && map.v.owned==PicassoCaptureMap && map.v.dma.retained);
    assert(!picasso_capture_rearm(map.v));
    Fixture close;close.until(PicassoCaptureRead);picasso_capture_cancel(close.v);unsigned phase=close.v.phase;picasso_capture_cancel(close.v);assert(close.v.phase==phase);
    assert(picasso_capture_due(close.v));picasso_capture_sent(close.v);
    for(unsigned i=0;i<102 && close.v.phase==PicassoCaptureClose;++i)picasso_capture_tick(close.v,true);
    close.finish();assert(close.v.cleanupError==-60 && (close.v.owned&PicassoCaptureStream));assert(close.hwStop==0 && close.offCommands==0);
    Fixture transport;transport.until(PicassoCaptureRead);uint32_t owned=transport.v.owned;
    picasso_capture_tick(transport.v,false);assert(transport.v.done && transport.v.transportLost && transport.v.owned==owned && transport.v.dma.retained);
    assert(!transport.hwStop && !transport.offCommands);
    Fixture backend;backend.completeError=-84;backend.finish();assert(backend.v.firstError==-84 && backend.v.dmaError==-84 && !backend.v.owned);
    Fixture hw;hw.hardwareError=-5;hw.finish();assert(hw.v.firstError==-5 && !hw.v.owned && hw.hwStop==1 && hw.offCommands==4);
    Fixture stop;stop.stopError=-5;stop.finish();assert(stop.v.cleanupError==-5 && (stop.v.owned&PicassoCaptureHardware) && stop.offCommands==0);
    Fixture cancel;cancel.until(PicassoCaptureRead);picasso_capture_cancel(cancel.v);cancel.finish();assert(cancel.v.firstError==-125 && !cancel.v.owned && picasso_capture_rearm(cancel.v));
    Fixture wait;for(unsigned i=0;i<252;++i)picasso_capture_tick(wait.v,false);assert(wait.v.done && !wait.v.attempted && wait.v.firstError==-60 && !wait.hwPrepare);
}
static void scheduling() {
    Fixture f;f.until(PicassoCapturePrime);assert(picasso_capture_due(f.v));unsigned prepared=f.prepared;
    assert(picasso_capture_due(f.v) && f.prepared==prepared && !f.v.dma.busyMask);
    picasso_capture_sent(f.v);assert(f.v.inflight==1 && f.v.dma.busyMask==1);
    assert(picasso_capture_due(f.v));picasso_capture_sent(f.v);assert(f.v.inflight==3 && !picasso_capture_due(f.v));
    f.finish();assert(f.v.complete);
    Fixture lost;lost.until(PicassoCaptureOpenCopp);assert(picasso_capture_due(lost.v));picasso_capture_sent(lost.v);
    for(unsigned i=0;i<102 && lost.v.phase==PicassoCaptureOpenCopp;++i)picasso_capture_tick(lost.v,true);
    lost.finish();assert(lost.v.firstError==-60 && (lost.v.owned&PicassoCaptureCopp) && !lost.v.coppValid);
    assert(lost.hwStop==0 && lost.offCommands==0);
    Fixture late;late.until(PicassoCaptureRead);picasso_capture_cancel(late.v);assert(picasso_capture_due(late.v));picasso_capture_sent(late.v);
    uint32_t b[11];late.payload(0,b);b[0]=1;unsigned phase=late.v.phase;
    assert(late.deliver(0,b));assert(late.v.phase==phase && late.v.pending && late.v.inflight==2);
    late.control();late.finish();assert(!late.v.owned && late.v.cleanupError==-5);
}
static void health() {
    Fixture f;f.until(PicassoCaptureRead);
    // Eight completed reads do not invoke hardware from receive dispatch.
    for(unsigned i=0;i<8;++i) {
        unsigned slot=(f.v.inflight&1)?0:1;uint32_t b[11];f.payload(slot,b);assert(f.deliver(slot,b));
        assert(!f.hwHealth);
        if(i<7) {picasso_capture_tick(f.v,true);if(picasso_capture_due(f.v))picasso_capture_sent(f.v);}
    }
    assert(f.v.readDone==8);f.healthPendingReturns=2;
    picasso_capture_tick(f.v,true);assert(f.v.healthPending && f.hwHealth==1 && picasso_capture_due(f.v));
    // A queued READ_DONE is still delivered while health waits asynchronously.
    assert(f.v.inflight);unsigned slot=(f.v.inflight&1)?0:1;uint32_t b[11];f.payload(slot,b);assert(f.deliver(slot,b));
    assert(f.v.readDone==9 && f.hwHealth==1);
    picasso_capture_tick(f.v,true);assert(f.v.healthPending && f.hwHealth==2 && picasso_capture_due(f.v));
    picasso_capture_tick(f.v,true);assert(!f.v.healthPending && f.hwHealth==3 && f.v.healthDone==9);
    f.finish();assert(f.v.complete && !f.v.owned);
    Fixture failed;failed.until(PicassoCaptureRead);failed.v.readDone=8;failed.healthError=-19;
    picasso_capture_tick(failed.v,true);assert(failed.v.firstError==-19 && failed.v.phase==PicassoCaptureClose);
    failed.finish();assert(!failed.v.complete && !failed.v.owned && failed.hwStop==1);
    Fixture timeout;timeout.until(PicassoCaptureRead);timeout.v.readDone=8;timeout.healthPendingReturns=1000;
    picasso_capture_tick(timeout.v,true);assert(timeout.v.healthPending);
    // Preserve the synthetic live slots while exercising the health deadline.
    for(unsigned i=0;i<100;++i) {timeout.v.readWait[0]=timeout.v.readWait[1]=0;picasso_capture_tick(timeout.v,true);}
    assert(timeout.v.firstError==-60 && !timeout.v.healthPending && timeout.v.phase==PicassoCaptureClose);
    timeout.finish();assert(!timeout.v.owned && timeout.hwStop==1);
    Fixture cancel;cancel.until(PicassoCaptureRead);cancel.v.readDone=8;cancel.healthPendingReturns=1;
    picasso_capture_tick(cancel.v,true);assert(cancel.v.healthPending);picasso_capture_cancel(cancel.v);
    cancel.healthError=-5;picasso_capture_tick(cancel.v,true);
    assert(cancel.v.phase==PicassoCaptureClose && cancel.v.firstError==-125 && cancel.v.cleanupError==-5);
    cancel.finish();assert(!cancel.v.owned);
}
static void streaming() {
    Fixture f;f.v.streaming=true;f.until(PicassoCaptureRead);
    for(unsigned i=0;i<1600;++i)f.step(); // More than the former 20s deadline.
    assert(!f.v.done && f.v.bytesDone>48000*2*10 && !f.v.firstError);
    unsigned wait=f.v.readWait[0],ticks=f.v.streamTicks;
    for(unsigned i=0;i<1000;++i)picasso_capture_tick(f.v,true,false);
    assert(f.v.readWait[0]==wait && f.v.streamTicks==ticks);
    uint32_t old=picasso_capture_read_token(f.v,f.v.readSent-1);
    picasso_capture_cancel(f.v);f.finish();assert(picasso_capture_rearm(f.v));
    assert(f.v.streaming && picasso_capture_read_token(f.v,0)>old);
    f.until(PicassoCaptureRead);uint32_t b[11];f.payload(0,b);
    assert(!f.deliver(0,b,44,old));assert(f.deliver(0,b));
    f.v.readSent=UINT32_MAX-f.v.readTokenBase;
    assert(!picasso_capture_due(f.v) && f.v.firstError==-75);
    f.finish();assert(!f.v.owned);
}
int main() {streaming();health();scheduling();wire();lifecycle();malformed();controlQualification();failures();puts("capture wire, lifecycle, short periods, ownership, malformed replies and failures PASS");}
