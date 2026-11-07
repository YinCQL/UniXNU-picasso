// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "PicassoSmp2p.h"
#include "PicassoAdspBoot.h"
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *e);
static uint32_t ram[WLAN_SMEM_SIZE/4], mutexRam[1024], ipccRam[1024], audioRam[4][1024], tfaRam[7][4096];
static unsigned allocations,mappings,releases,unmaps,lines,sleeps,preempt,plans,kicks;
static unsigned failAlloc,failMap,zeroMap;
static bool peer,transport;
static bool checkShortLogs;
static constexpr unsigned part=0x1cf000, toc=WLAN_SMEM_SIZE-4096;
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,OSNoRetain=0};
class IOMemoryMap {
public:
    uintptr_t address;
    uintptr_t getVirtualAddress(){return address;}
    void release(){++unmaps;}
};
class IODeviceMemory {
public:
    unsigned slot;
    static IODeviceMemory *withRange(uint64_t base,uint64_t bytes) {
        unsigned at=base==WLAN_SMEM_BASE?0:base==0x01f43000?1:2;
        if(at==2 && base!=0x00408000) {
            const uint64_t pa[]={0xf546000,0xf547000,0xf548000,0xf54a000};
            unsigned i=0;for(;i<4 && base!=pa[i];++i){}
            if(i<4)at=3+i;
            else {
                const uint64_t tfa[]={0x988000,0x118000,0x123000,0x152000,0xf531000,0xf532000,0xf53a000};
                unsigned j=0;for(;j<7 && base!=tfa[j];++j){}assert(j<7);at=7+j;
            }
        }
        assert(bytes==(at==7?0x4000:at?4096:WLAN_SMEM_SIZE));
        static IODeviceMemory memory[14];memory[at].slot=at;
        return ++allocations==failAlloc?nullptr:&memory[at];
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags==3);static IOMemoryMap maps[14];++mappings;
        maps[slot].address=mappings==zeroMap?0:reinterpret_cast<uintptr_t>(slot==0?ram:slot==1?mutexRam:slot==2?ipccRam:slot<7?audioRam[slot-3]:tfaRam[slot-7]);
        return mappings==failMap?nullptr:&maps[slot];
    }
    void release(){++releases;}
};
template<class T> class OSSharedPtr {
    T *p;
public:
    OSSharedPtr(T *v,int):p(v){}
    ~OSSharedPtr(){if(p)p->release();}
    explicit operator bool()const{return p!=nullptr;}
    T *operator->()const{return p;}
};
static void IOLog(const char *fmt,...) {
    assert(!preempt);
    va_list a;va_start(a,fmt);
    if(checkShortLogs) {
        char text[1024];int n=vsnprintf(text,sizeof text,fmt,a);
        assert(n>0 && n<256 && text[n-1]=='\n');
    } else {vprintf(fmt,a);++lines;}
    va_end(a);
}
static void disable_preemption(){assert(!preempt);++preempt;}
static void enable_preemption(){assert(preempt==1);--preempt;}
static unsigned inbound;
static void IOSleep(unsigned ms) {
    assert(!preempt);++sleeps;
    if(ms==1)return;
    assert(ms==20 && !mutexRam[0]);
    if(ipccRam[3]==0x30002){++kicks;ipccRam[3]=0;}
    if(peer && sleeps==1) {
        unsigned at=part+ram[(part+12)/4];
        ram[at/4]=0xa5a5|(443U<<16);ram[at/4+1]=344;ram[at/4+2]=4;
        ram[(part+12)/4]+=360;inbound=at+16;
        ram[inbound/4]=0x504d5324;ram[inbound/4+1]=0x101;
        ram[inbound/4+2]=2;ram[inbound/4+3]=16;ram[inbound/4+4]=1;
    }
    if(peer && sleeps==2) {
        memcpy(reinterpret_cast<uint8_t *>(ram)+inbound+20,"slave-kernel",13);
        ram[inbound/4+9]=2;ram[inbound/4+3]=16|(1U<<16);
    }
    if(peer && sleeps>2)ram[inbound/4+9]^=1; // Exercise the bounded event log.
    if(sleeps==1602)ram[(part+48+36)/4]=1; // External mutation stops service.
}
static void IODelay(unsigned) {}
#include "PicassoSmp2pDevice-body.inc"
extern "C" { volatile uint32_t native_novatek_touch_stage=0; }
extern "C" void picasso_pcm_device_prepare(PicassoPcm &) { assert(false && "unexpected PCM DMA without ASM completion"); }
extern "C" void picasso_rpc_device_prepare(PicassoFastrpc *) { assert(false && "unexpected RPC DMA without an open channel"); }
// Exercise the real SMEM/MMIO adapter separately from the coordinator tests,
// which prove that this callback is gated by PAS authentication and fresh ready.
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *e) {
    assert(e && e->poll && e->snapshot && e->service);
    if(!transport)return;
    auto *b=static_cast<BootEvents *>(e->context);auto *g=b->glink;
    assert(!preempt && !mutexRam[0] && g->prepared && !g->sent);
    assert(ipccRam[3]==0x30002);++kicks;ipccRam[3]=0;
    PicassoSmp2pEvent event={};assert(e->poll(e->context,&event)==-2);
    assert(!g->sent && !ram[(g->desc.offset+4)/4] && !ipccRam[3]);
    e->service(e->context);
    assert(g->sent && !g->ready && g->txHead==8);
    assert(ram[g->tx.offset/4]==0x10000 && !ram[g->tx.offset/4+1]);
    assert(ipccRam[3]==0x30000);ipccRam[3]=0;
    unsigned at=part+ram[(part+12)/4],rx=at+16;
    ram[at/4]=0xa5a5|(480U<<16);ram[at/4+1]=16384;
    ram[rx/4]=0x10000;ram[rx/4+1]=1; // peer offers an optional feature
    ram[rx/4+2]=0x10001;ram[rx/4+3]=0; // agrees to features 0
    ram[(part+12)/4]+=16400;ram[(g->desc.offset+12)/4]=16;
    e->service(e->context);
    assert(g->ready && g->rxTail==16 && g->txHead==16);
    assert(ram[g->tx.offset/4+2]==0x10001 && !ram[g->tx.offset/4+3]);
    assert(ipccRam[3]==0x30000);ipccRam[3]=0;
    e->service(e->context);
    assert(ipccRam[3]==0x30000 && g->channel[0].openSent && g->txHead==40);
    assert(ram[(g->tx.offset+16)/4]==0x10002 && ram[(g->tx.offset+20)/4]==14);
    assert(!memcmp(reinterpret_cast<uint8_t *>(ram)+g->tx.offset+24,"apr_audio_svc",14));
    ipccRam[3]=0;
    e->service(e->context);assert(!ipccRam[3]);
    // The real adapter accepts a DSP-owned ID zero, publishes it unchanged in
    // APR TX_DATA, and only marks ready after an actual matching RX payload.
    unsigned cursor=16;
    auto append=[&](const uint32_t *words,unsigned bytes){
        memcpy(reinterpret_cast<uint8_t *>(ram)+rx+cursor,words,bytes);
        cursor+=bytes;ram[(g->desc.offset+12)/4]=cursor;
    };
    uint32_t open[6]={0x40002,14};memcpy(open+2,"apr_audio_svc",14);append(open,sizeof open);
    const uint32_t ack[2]={0x10004,0};append(ack,sizeof ack);
    e->service(e->context);assert(!g->channel[0].up); // our OPEN_ACK not sent yet
    ipccRam[3]=0;e->service(e->context);assert(g->channel[0].up && ipccRam[3]==0x30000);
    ram[g->desc.offset/4]=g->txHead;ipccRam[3]=0;
    const uint32_t offer[4]={0x40005,1,512,0};append(offer,sizeof offer);
    e->service(e->context);assert(g->remoteIntent[0].used && !g->remoteIntent[0].id && !g->apr.pending);
    ram[g->desc.offset/4]=g->txHead;ipccRam[3]=0;unsigned tx=g->txHead;
    e->service(e->context);assert(g->apr.pending && g->remoteIntent[0].busy && ipccRam[3]==0x30000);
    assert(ram[(g->tx.offset+tx+16)/4]==0x10009 && !ram[(g->tx.offset+tx+20)/4]);
    const uint32_t reuse[2]={0x4000e,0};append(reuse,sizeof reuse);e->service(e->context);
    assert(g->remoteIntent[0].used && !g->remoteIntent[0].busy && g->apr.pending && g->remoteReuse==1);
    const uint32_t reply[14]={0x40009,1,36,0,0x00240081,0x403,0x503,g->apr.token,0x1290d,0xaaaa,0xbbbb,0xcccc,5,0};
    append(reply,sizeof reply);e->service(e->context);assert(g->apr.ready && g->apr.state==5);
    ram[g->desc.offset/4]=g->txHead;tx=g->txHead;e->service(e->context);
    assert(ram[(g->tx.offset+tx)/4]==0x1000e && ram[(g->tx.offset+tx+4)/4]==1);
    assert(g->localReuse==1 && g->localIntent[0].id==1 && g->localIntent[0].advertised);ipccRam[3]=0;
}
extern "C" int picasso_rpmh_adsp_plan(){assert(!preempt && !allocations);++plans;return 0;}
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];
    ram[0x5c/4]=12U<<16;ram[toc/4]=0x434f5424;ram[toc/4+1]=1;ram[toc/4+2]=1;
    ram[toc/4+8]=part;ram[toc/4+9]=0x30000;ram[toc/4+11]=2U<<16;ram[toc/4+12]=64;
    ram[part/4]=0x54525024;ram[part/4+1]=2U<<16;ram[part/4+2]=0x30000;
    ram[part/4+3]=32;ram[part/4+4]=0x30000;
    // Include the two real XBL reservations in the device-adapter fixture.
    ram[toc/4+2]=3;
    ram[(toc+80)/4]=toc;ram[(toc+84)/4]=4096;ram[(toc+88)/4]=1;ram[(toc+92)/4]=0xffffffff;
    ram[(toc+132)/4]=4096;ram[(toc+136)/4]=3;ram[(toc+140)/4]=0xffffffff;
    if(!strncmp(mode,"alloc",5))failAlloc=unsigned(mode[5]-'0');
    if(!strncmp(mode,"map",3))failMap=unsigned(mode[3]-'0');
    if(!strncmp(mode,"zero",4))zeroMap=unsigned(mode[4]-'0');
    if(!strcmp(mode,"owner"))mutexRam[0]=1;
    if(!strcmp(mode,"remote"))mutexRam[0]=2;
    if(!strcmp(mode,"gate"))allocatorGate=1;
    if(!strcmp(mode,"bad"))ram[part/4]=0;
    peer=!strcmp(mode,"peer");
    transport=!strcmp(mode,"glink");
    // Exercise aligned word reads, arbitrary output alignment and boundaries.
    SmemDevice d={reinterpret_cast<uintptr_t>(ram),reinterpret_cast<uintptr_t>(mutexRam),0,false};
    uint8_t data[11]={};assert(!smemRead(&d,0x5d,data+1,7));
    assert(!memcmp(data+1,reinterpret_cast<uint8_t *>(ram)+0x5d,7));
    assert(smemRead(&d,WLAN_SMEM_SIZE-1,data,2)==-22);
    picasso_smp2p_start();picasso_smp2p_start();
    assert(allocations==3 && releases==3-!!failAlloc && mappings==releases && unmaps==mappings-!!failMap && !preempt);
    if(!strcmp(mode,"normal") || peer || transport) {
        assert(plans==1 && sleeps==1602 && kicks==(peer?2U:1U));
    assert(lines==(peer?71U:transport?22U:9U));
        assert(ram[(part+12)/4]==(peer?17200U:transport?33240U:16840U));
        if(transport)assert(!ipccRam[3]); // stopped observer never transmits GLINK
        assert(!allocatorGate && !mutexRam[0]);
    } else {
        assert(plans==1 && !kicks && ram[(part+12)/4]==32);
        assert(sleeps==(!strcmp(mode,"remote")?100U:0U));
        if(strcmp(mode,"gate"))assert(!allocatorGate);
    }
    puts("PASS SMP2P device mappings, local/remote ownership, one-shot gate, persistent poll/negotiation and bounded logging");
    failAlloc=failMap=zeroMap=0;
    unsigned oldAlloc=allocations;uint32_t value=0;
    assert(audioPinRead(nullptr,4,0,&value)==-22 && audioPinRead(nullptr,0,8,&value)==-22);
    assert(allocations==oldAlloc);
    for(unsigned i=0;i<4;++i) {
        audioRam[i][0]=0x2800;audioRam[i][1]=1;
        assert(!audioPinRead(nullptr,i,0,&value) && value==0x2800);
        assert(!audioPinWrite(nullptr,i,4,2) && audioRam[i][1]==2);
    }
    failAlloc=allocations+1;assert(audioPinRead(nullptr,0,0,&value)==-12);failAlloc=0;
    failMap=mappings+1;assert(audioPinWrite(nullptr,0,4,0)==-12);failMap=0;
    zeroMap=mappings+1;assert(audioPinRead(nullptr,0,0,&value)==-12);zeroMap=0;
    assert(audioRam[0][1]==2);
    PicassoAudioPins pins={};pins.read=audioPinRead;pins.write=audioPinWrite;
    assert(!picasso_audio_pins_activate(pins) && pins.active);
    picasso_audio_pins_restore(pins);assert(pins.restored && !pins.dirty);
    for(unsigned i=0;i<4;++i)assert(audioRam[i][0]==0x2800 && audioRam[i][1]==2);
    puts("PASS four exact audio GPIO mappings, invalid offsets, allocation/map/zero-VA errors and device adapter restoration");
    PicassoTfa tfa={};unsigned tfaAlloc=allocations;
    tfaProbe(tfa);assert(tfa.error==-16 && allocations==tfaAlloc);
    native_novatek_touch_stage=2;
    for(unsigned failure=0;failure<3;++failure)for(unsigned i=1;i<=7;++i) {
        tfa={};if(failure==0)failAlloc=allocations+i;
        if(failure==1)failMap=mappings+i;
        if(failure==2)zeroMap=mappings+i;
        tfaProbe(tfa);assert(tfa.error==-12 && !tfa.complete && !tfa.commands);
        failAlloc=failMap=zeroMap=0;
    }
    tfa={};tfaProbe(tfa);assert(tfa.error==-16 && tfa.stage==1 && !tfa.commands && tfa.restored);
    TfaMaps tm={};for(unsigned i=0;i<7;++i)tm.address[i]=reinterpret_cast<uintptr_t>(tfaRam[i]);
    assert(tfaRead(&tm,7,0,&value)==-22 && tfaRead(&tm,1,4096,&value)==-22);
    assert(tfaRead(&tm,0,0,nullptr)==-22 && tfaWrite(&tm,0,3,1)==-22);
    assert(!tfaWrite(&tm,0,0x270,2) && !tfaRead(&tm,0,0x270,&value) && value==2);
    puts("PASS SE8 maps, touch-owner gate, every allocation/map/zero-VA failure and MMIO boundaries");
    checkShortLogs=true;
    tfa.stage=tfa.events=tfa.fw=tfa.ifDisable=tfa.hw[0]=tfa.hw[1]=0xffffffff;
    tfa.clockCfg=tfa.clockLevel=tfa.irq=tfa.rxStatus=tfa.ios=tfa.commands=tfa.reg=tfa.abortStatus=0xffffffff;
    tfa.error=tfa.cleanupError=-2147483647;tfa.identified=true;tfa.revision=0xffff;
    for(auto &v:tfa.values)v=0xffff;for(auto &v:tfa.pinBefore)v=0xffffffff;for(auto &v:tfa.pinAfter)v=0xffffffff;logTfa(tfa);
    pins.attempted=pins.active=pins.restored=true;pins.dirty=0xffffffff;
    pins.error=pins.restoreError=-2147483647;for(auto &v:pins.observed)v=0xffffffff;logAudioPins(pins);
    tfa.complete=true;tfa.registerWrites=tfa.digitalDirty=0xffffffff;
    tfa.digitalError=tfa.digitalRestoreError=-2147483647;for(auto &v:tfa.digital)v=0xffff;logTfa(tfa);
    PicassoAprProbe stream={};stream.stream.phase=stream.stream.token=stream.stream.api=stream.stream.status=0xffffffff;
    stream.stream.error=stream.stream.openError=stream.stream.formatError=-2147483647;
    for(auto &v:stream.stream.statuses)v=0xffffffff;logStream(stream);
    auto &pcm=stream.pcm;pcm.phase=pcm.token=pcm.opcode=pcm.status=pcm.handle=pcm.sid=pcm.bytes=pcm.bank=pcm.asid=pcm.fsr=pcm.farHigh=pcm.farLow=pcm.syndrome=0xffffffff;
    pcm.address=~0ULL;pcm.error=pcm.mapError=pcm.dmaError=-2147483647;
    pcm.statuses[0]=pcm.statuses[1]=0xffffffff;logPcm(stream);
    auto &play=stream.play;play.phase=play.token=play.opcode=play.status=play.copp=play.handle=play.accepted=play.owned=0xffffffff;
    play.writeAddressHigh=play.writeAddressLow=play.writeHandle=play.writeStatus=0xffffffff;
    play.replyPhase=play.replyOpcode=play.replyType=play.replyBytes=play.replySource=play.replyDestination=play.replyReject=play.rejected=0xffffffff;
    for(auto &word:play.replyRaw)word=0xffffffff;
    play.error=play.firstError=play.cleanupError=play.dmaError=-2147483647;logPlay(stream);
    PicassoRpcDomain record={};record.pd=record.requests=record.lastHandle=record.lastScalar=0xffffffffU;
    record.wireBytes=record.decodeResult=record.previousResult=0xffffffffU;
    memset(record.name,'n',127);memset(record.fs.target,'t',127);
    record.fs.nodes=record.fs.nodeLimit=record.fs.tombstones=record.fs.bytes=0xffffffffU;
    record.fs.blocks=record.fs.blockLimit=record.fs.exhaustion=0xffffffffU;
    logRpcRequest(record,0xffffffffU);
    PicassoAprProbe apr={};apr.afe.valid=true;
    apr.afe.token=apr.afe.opcode=apr.afe.api=apr.afe.status=0xffffffffU;apr.afe.error=-2147483647;
    for(auto &v:apr.afe.config)v=0xffffffffU;
    apr.afe.payloadBytes=48;apr.afe.headerBytes=32;apr.afe.rejected=4;
    for(auto &v:apr.afe.raw)v=0xffffffffU;logAfe(apr);
    PicassoRpcIdle idle={};for(unsigned n=0;n<1000;++n)assert(!picasso_rpc_idle(idle,0));
    assert(!picasso_rpc_idle(idle,1287));for(unsigned n=0;n<99;++n)assert(!picasso_rpc_idle(idle,1287));
    assert(picasso_rpc_idle(idle,1287));for(unsigned n=0;n<1000;++n)assert(!picasso_rpc_idle(idle,1287));
    assert(!picasso_rpc_idle(idle,1288));for(unsigned n=0;n<50;++n)assert(!picasso_rpc_idle(idle,1288));
    assert(!picasso_rpc_idle(idle,1289));for(unsigned n=0;n<99;++n)assert(!picasso_rpc_idle(idle,1289));
    assert(picasso_rpc_idle(idle,1289));
    puts("PASS RPC idle transitions and worst-case short request/path/target/capacity/AFE log records");
}
