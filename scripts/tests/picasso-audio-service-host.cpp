// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual service/worker body under a narrow host IOKit model. SDK checked separately.
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "PicassoApr.h"
using IOReturn=int;using OSReturn=int;using UInt32=unsigned;using task_t=void *;
struct OSObject {};struct OSDictionary {};struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
    uint32_t scalarInputCount=0,scalarOutputCount=0,structureInputSize=0,structureOutputSize=0;
    void *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    uint32_t asyncWakePort=0;const void *structureInput=nullptr;void *structureOutput=nullptr;
};
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnNotOpen=-3,
    kIOReturnNotPermitted=-4,kIOReturnNoSpace=-5,kIOReturnBusy=-6};
static const char *kIOClientPrivilegeAdministrator="root";
using proc_t=void *;
static bool platformAudio=false;
static const char *clientIdentity="com.apple.mediaserverd";
static proc_t proc_self(){return reinterpret_cast<void *>(1);}
static task_t proc_task(proc_t){return nullptr;}
static const char *csproc_get_identity(proc_t){return clientIdentity;}
static int csproc_get_platform_binary(proc_t){return platformAudio;}
static void proc_rele(proc_t){}
static bool privileged=true;static unsigned registrations,metaCalls,lockDepth;
struct IOLock {};
static IOLock *IOLockAlloc(){static IOLock l;return &l;}
static void IOLockLock(IOLock *) {assert(!lockDepth);++lockDepth;}
static void IOLockUnlock(IOLock *) {assert(lockDepth==1);--lockDepth;}
class IOService:public OSObject {
public:
    virtual bool start(IOService *) {return true;}
    bool init(){return true;}bool attach(IOService *){return true;}void detach(IOService *){}void release(){}void terminate(){}
    bool setProperty(const char *,const char *){return true;}bool setProperty(const char *,uint64_t,unsigned){return true;}
    void registerService(){++registrations;}static IOService *getResourceService(){static IOService p;return &p;}
};
class IOUserClient:public IOService {
public:
    virtual bool initWithTask(task_t,void *,UInt32,OSDictionary *){return true;}
    virtual IOReturn clientClose(){return 0;}virtual IOReturn clientDied(){return clientClose();}
    virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *){return -1;}
    static IOReturn clientHasPrivilege(void *,const char *){return privileged?0:-1;}
};
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
#define OSDynamicCast(x,y) static_cast<x *>(y)
template<class T> static T *allocObject(){static T object;return &object;}
#define OSTypeAlloc(x) allocObject<x>()
struct OSMetaClass {static void *preModLoad(const char *) {return reinterpret_cast<void *>(1);}static int postModLoad(void *){return 0;}};
struct kmod_info_t {char name[16];};
extern "C" {kmod_info_t g_kernel_kmod_info={"kernel"};}
extern "C" void picasso_audio_static_init(){++metaCalls;}
static void IOLog(const char *fmt,...) {
    char b[1024];va_list a;va_start(a,fmt);int n=vsnprintf(b,sizeof b,fmt,a);va_end(a);assert(n>0 && n<256);
}
#include "PicassoAudioService-body.inc"
static uint32_t pin[4][2];static unsigned copies;static uint64_t copied;
static int pinRead(void *,unsigned i,unsigned reg,uint32_t *v){*v=pin[i][reg/4];return 0;}
static int pinWrite(void *,unsigned i,unsigned reg,uint32_t v){pin[i][reg/4]=v;return 0;}
static int health(PicassoPcm &){return 0;}
static int copyPcm(PicassoPcm &p,unsigned slot,const uint8_t *data,unsigned bytes) {
    assert(!lockDepth && slot<2 && data && bytes && bytes<=16384 && !(bytes&3) && !(p.busyMask&(1U<<slot)));
    ++copies;copied+=bytes;return 0;
}
static int amplifier(PicassoPlayback &v,unsigned action) {
    assert(!lockDepth);if(action==0)v.amp.attempted=v.amp.configured=true;
    if(action==1)v.amp.enabled=true;if(action==2){v.amp.enabled=false;v.amp.stopped=true;}return 0;
}
static void ack(PicassoAprProbe &p) {
    auto &v=p.play;uint32_t w[18];picasso_play_request(p,w);uint32_t b[4]={w[4],0,0,0};unsigned bytes=8,type=1;
    p.source=w[2];p.destination=w[1];p.receivedToken=v.token;p.opcode=0x110e8;
    if(v.phase==PicassoPlayMapBuffer) {p.opcode=0x10d93;b[0]=0x7788;bytes=4;}
    if(v.phase==PicassoPlayOpenCopp) {p.opcode=0x10329;b[0]=b[1]=0;}
    if(v.phase==PicassoPlayWrite) {
        unsigned slot=v.inflight&1?0:1;p.receivedToken=picasso_play_base(v)+0x1001+v.writeSequence[slot];
        p.opcode=0x10d99;type=0;bytes=16;b[0]=0x80000000+slot*16384;b[1]=1;b[2]=v.handle;b[3]=0;
    }
    if(v.phase==PicassoPlayDrain) {p.opcode=0x10c1c;type=0;bytes=0;}
    assert(picasso_play_receive(p,reinterpret_cast<const uint8_t *>(b),bytes,type));
}
static void cycle(PicassoAprProbe &p,PicassoQmi &q) {
    picasso_audio_service_tick(p,false);picasso_play_tick(p,q);
    if(picasso_play_due(p))picasso_play_sent(p);
    if(p.play.pending)ack(p);
}
static IOReturn call(PicassoAudioUserClient &c,unsigned selector,const void *data=nullptr,unsigned bytes=0,void *out=nullptr,unsigned outBytes=0) {
    IOExternalMethodArguments a;a.structureInput=data;a.structureInputSize=bytes;a.structureOutput=out;a.structureOutputSize=outBytes;
    return c.externalMethod(selector,&a,nullptr,nullptr,nullptr);
}
static void queueTests() {
    static PicassoAudioQueue q;static uint8_t data[4096];for(unsigned i=0;i<4096;++i)data[i]=uint8_t(i*7);
    assert(picasso_audio_begin(q,1,format)==-6);q.ready=true;
    auto bad=format;bad.rate=44100;assert(picasso_audio_begin(q,1,bad)==-22);
    assert(!picasso_audio_begin(q,7,format));assert(picasso_audio_begin(q,8,format)==-16);
    assert(picasso_audio_write(q,8,data,4)==-1 && picasso_audio_write(q,7,data,4097)==-22);
    assert(picasso_audio_end(q,7,false)==-22);
    for(unsigned i=0;i<32;++i)assert(!picasso_audio_write(q,7,data,4096));
    assert(picasso_audio_write(q,7,data,4)==-11 && q.used==131072 && q.submitted==131072);
    for(unsigned i=0;i<131072;++i)assert(q.data[i]==data[i%4096]);
    q.read=131068;q.write=131068;q.used=0;
    assert(!picasso_audio_write(q,7,data,12) && q.write==8 && q.data[0]==data[4]);
    assert(!picasso_audio_available(q));assert(!picasso_audio_end(q,7,false) && picasso_audio_available(q)==12);
    assert(picasso_audio_write(q,7,data,4)==-16);
    picasso_audio_retire(q,true,0,true);assert(q.drained && !q.owner && !q.used && q.state==0);
    assert(!picasso_audio_begin(q,8,format));assert(!picasso_audio_end(q,8,true) && !picasso_audio_available(q));
    picasso_audio_retire(q,false,-5,false);assert(picasso_audio_begin(q,9,format)==-6);
}
int main(int argc,char **) {
    queueTests();static PicassoAprProbe p;PicassoQmi q={};q.bound=true;q.round=1;
    for(auto &d:q.domains){d.completed=d.stateValid=true;d.state=0x1fffffff;}
    p.ready=p.fw.valid=true;p.fw.count=1;p.fw.services[0][0]=8;p.fw.services[0][1]=4;
    p.pcm.prepared=p.pcm.done=p.pcm.complete=true;p.pcm.address=0x180000000ULL;p.pcm.bytes=32768;p.pcm.check=health;p.pcm.copy=copyPcm;
    p.pins.read=pinRead;p.pins.write=pinWrite;p.tfa.digitalApplied=p.tfa.digitalRestored=true;
    if(argc>1){
        p.play.amplifier=amplifier;
        p.pcm.uncertain=true;picasso_audio_service_tick(p,false);assert(!published && !queueLock);p.pcm.uncertain=false;
        p.tfa.digitalDirty=true;picasso_audio_service_tick(p,false);assert(!published && !queueLock);p.tfa.digitalDirty=false;
        p.pcm.busyMask=1;picasso_audio_service_tick(p,false);assert(!published && !queueLock);p.pcm.busyMask=0;
        picasso_audio_service_tick(p,false);
        assert(published && testPhase==3 && !active && !p.play.attempted && !p.play.complete && !queue.epoch && !copies);
        PicassoAudio provider;PicassoAudioUserClient client;
        assert(client.initWithTask(nullptr,nullptr,0,nullptr)&&client.start(&provider));
        static uint8_t data[4096];
        assert(!call(client,1,&format,sizeof format));
        for(unsigned i=0;i<4;++i)assert(!call(client,2,data,sizeof data));
        assert(!call(client,3));
        for(unsigned i=0;i<80;++i)cycle(p,q);
        assert(queue.epoch==1 && queue.returned==16384 && queue.drained && p.play.complete && !active);
        puts("PASS quiet boot: no test PCM, real first client playback/drain");return 0;
    }
    p.play.done=p.play.complete=p.play.amp.stopped=true;p.play.amplifier=amplifier;
    for(unsigned i=0;i<300 && !published;++i)cycle(p,q);
    assert(published && registrations==1 && metaCalls==1 && testPhase==3 && !active);
    assert(queue.epoch==2 && queue.returned==96000 && queue.drained && copied==288000 && copies==18);
    assert(picasso_play_base(p.play)==0x20590000 && !p.pcm.busyMask && p.play.complete);
    PicassoAudio provider;PicassoAudioUserClient c,other;
    privileged=false;assert(!c.initWithTask(nullptr,nullptr,0,nullptr));privileged=true;
    privileged=false;platformAudio=true;assert(c.initWithTask(nullptr,nullptr,0,nullptr));
    clientIdentity="com.example.spoof";assert(!c.initWithTask(nullptr,nullptr,0,nullptr));
    clientIdentity="com.apple.mediaserverd";platformAudio=false;privileged=true;
    assert(!c.initWithTask(nullptr,nullptr,1,nullptr));assert(c.initWithTask(nullptr,nullptr,0,nullptr) && c.start(&provider));
    assert(other.initWithTask(nullptr,nullptr,0,nullptr) && other.start(&provider));
    IOExternalMethodArguments malformed;malformed.scalarInputCount=1;
    assert(c.externalMethod(1,&malformed,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    PicassoAudioStatus status;assert(!call(c,0,nullptr,0,&status,sizeof status) && status.version==1 && status.epoch==2);
    assert(call(c,99)==kIOReturnBadArgument && call(c,0,nullptr,0,&status,4)==kIOReturnBadArgument);
    PicassoAudioClientNote note={1,1,0,0,0,0,0,0,0};
    assert(!call(c,PicassoAudioNote,&note,sizeof note));
    for(unsigned kind=7;kind<=14;++kind){note.kind=kind;assert(!call(c,PicassoAudioNote,&note,sizeof note));}
    note.kind=15;assert(call(c,PicassoAudioNote,&note,sizeof note)==kIOReturnBadArgument);
    note.kind=1;note.reserved=1;
    assert(call(c,PicassoAudioNote,&note,sizeof note)==kIOReturnBadArgument);
    assert(call(c,PicassoAudioNote,&note,sizeof note-4)==kIOReturnBadArgument);
    assert(!call(c,1,&format,sizeof format));assert(call(other,1,&format,sizeof format)==kIOReturnBusy);
    static uint8_t data[4096];memset(data,0x25,sizeof data);
    assert(call(other,2,data,4096)==kIOReturnNotPermitted);
    for(unsigned i=0;i<32;++i)assert(!call(c,2,data,4096));
    assert(call(c,2,data,4096)==kIOReturnNoSpace);
    // More than 24 chunks, then EOS: source is no longer a fixed tone count.
    for(unsigned i=0;i<300;++i) {cycle(p,q);if(i<100) {auto rc=call(c,2,data,4096);assert(!rc || rc==kIOReturnNoSpace);}if(i==100)assert(!call(c,3));}
    assert(!active && queue.state==PicassoAudioIdle && queue.drained && queue.returned==queue.submitted && queue.replies>24);
    assert(!call(c,1,&format,sizeof format));assert(!call(c,2,data,4096));assert(!call(c,3));
    for(unsigned i=0;i<40;++i)cycle(p,q);assert(!active && queue.returned==4096 && queue.drained);
    // Close while a real stream runs: stop and release only on DSP close ACK.
    assert(!call(c,1,&format,sizeof format));for(unsigned i=0;i<8;++i)assert(!call(c,2,data,4096));
    for(unsigned i=0;i<7;++i)cycle(p,q);assert(active);assert(!c.clientDied());
    for(unsigned i=0;i<40;++i)cycle(p,q);
    assert(!active && queue.state==PicassoAudioIdle && !queue.cancel && !queue.drained && !p.pcm.busyMask && !p.play.complete);
    assert(call(c,1,&format,sizeof format)==kIOReturnNotOpen);
    assert(!call(other,1,&format,sizeof format));for(unsigned i=0;i<4;++i)assert(!call(other,2,data,4096));assert(!call(other,3));
    for(unsigned i=0;i<50;++i)cycle(p,q);assert(!active && queue.drained && queue.returned==16384);
    // Late replies from the previous session cannot acquire a fresh handle.
    uint32_t oldBase=picasso_play_base(p.play);
    assert(!call(other,1,&format,sizeof format));for(unsigned i=0;i<4;++i)assert(!call(other,2,data,4096));assert(!call(other,3));
    picasso_audio_service_tick(p,false);picasso_play_tick(p,q);assert(picasso_play_due(p));picasso_play_sent(p);
    uint32_t late[2]={0x100ef,0};p.source=0x404;p.destination=0x504;p.opcode=0x110e8;p.receivedToken=oldBase+1;
    assert(!picasso_play_receive(p,reinterpret_cast<uint8_t *>(late),8,1) && p.play.pending && !p.play.accepted);
    ack(p);for(unsigned i=0;i<50;++i)cycle(p,q);assert(queue.drained);
    // Source starvation stops without inventing returned data or an EOS.
    assert(!call(other,1,&format,sizeof format));for(unsigned i=0;i<4;++i)assert(!call(other,2,data,4096));
    for(unsigned i=0;i<160;++i)cycle(p,q);
    assert(!active && queue.state==PicassoAudioFailed && queue.error==-60 && !queue.drained && queue.returned==16384);
    // Restore the fixture's previous healthy idle state only for independent
    // empty-start testing; the production service latches this fault.
    queue.state=PicassoAudioIdle;queue.ready=true;p.play.complete=true;p.play.firstError=0;
    // Empty start expires without touching hardware; transport loss latches unavailable.
    assert(!call(other,1,&format,sizeof format));for(unsigned i=0;i<252;++i)cycle(p,q);
    assert(queue.state==PicassoAudioIdle && queue.error==-60);
    assert(!call(other,1,&format,sizeof format));cycle(p,q);
    assert(queue.state==PicassoAudioStarting && startWait==1);
    assert(!call(other,4));cycle(p,q);assert(queue.state==PicassoAudioIdle && !queue.cancel);
    picasso_audio_service_tick(p,true);assert(!queue.ready && queue.state==PicassoAudioFailed && call(other,1,&format,sizeof format)==kIOReturnNotReady);
    static PicassoAprProbe stuck;p.play.complete=true;p.play.firstError=0;
    stuck.ready=stuck.fw.valid=true;stuck.play.attempted=true;stuck.play.phase=PicassoPlayWrite;
    stuck.play.source=audioSource;stuck.play.bufferReady=true;stuck.play.wait=250;
    picasso_play_tick(stuck,q);assert(stuck.play.firstError==-60 && stuck.play.phase>=PicassoPlayClose);
    static PicassoAprProbe unavailable;PicassoQmi down={};
    unavailable.play.enabled=true;unavailable.play.source=audioSource;
    for(unsigned i=0;i<251;++i)picasso_play_tick(unavailable,down);
    assert(unavailable.play.done && unavailable.play.firstError==-60 && !unavailable.play.owned);
    // A failed cancellation must stay failed after retire, without a sticky
    // cancel flag rewriting the recorded fault on every following tick.
    queue.cancel=true;picasso_audio_retire(queue,false,-5,false);
    picasso_audio_service_tick(p,false);assert(queue.state==PicassoAudioFailed && queue.error==-5);
    puts("PASS actual audio service: two reopens, real source PCM, >24 writes, partial tail, exclusive client, malformed calls, EOF, death/cancel, restart and unavailable transport");
}
