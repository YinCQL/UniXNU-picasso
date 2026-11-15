// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only WCD rail qualification. PMIC identity and observer mapping come
// from the Picasso DTS/SPMI v5 driver; unified power layout is checked against
// the board's original XBL/AOP register tables. No regulator vote is changed.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "PicassoMicPower.h"
#include "PicassoSpmi.h"
namespace {
using namespace picasso_mic_power;
IOLock *powerLock;
IOMemoryMap *map(uint64_t base,uint64_t size) {
    IODeviceMemory *memory=IODeviceMemory::withRange(base,size);
    if(!memory)return nullptr;
    IOMemoryMap *result=memory->map(kIOMapAnywhere|kIOMapInhibitCache);
    memory->release();return result;
}
struct Backend {
    IOMemoryMap *coreMap,*ownerMap,*channelMap[EndpointCount];
    volatile uint32_t *core,*owners,*channel[EndpointCount];
    Status status;
    bool poisoned;
    uint32_t table(bool owner,unsigned offset) {
        uint32_t value=(owner?owners:core)[offset/4];
        __asm__ volatile("dsb sy" ::: "memory");return value;
    }
    void close() {
        // A timed-out observer may still be completing. Never reuse that
        // channel or discard its diagnostic mapping during this boot.
        if(poisoned)return;
        for(unsigned i=0;i<EndpointCount;++i) {
            if(channelMap[i])channelMap[i]->release();
            channelMap[i]=nullptr;channel[i]=nullptr;
        }
        if(coreMap)coreMap->release();if(ownerMap)ownerMap->release();
        coreMap=ownerMap=nullptr;core=owners=nullptr;
    }
    int open() {
        status.stage=Controller;
        if(!picasso_spmi_init())return Unavailable;
        coreMap=map(0x0c440000,0x2000);ownerMap=map(0x0c40a000,0x1000);
        if(!coreMap || !ownerMap)return -12;
        core=reinterpret_cast<volatile uint32_t *>(coreMap->getVirtualAddress());
        owners=reinterpret_cast<volatile uint32_t *>(ownerMap->getVirtualAddress());
        if(!core || !owners)return -12;
        uint32_t version=table(false,0);
        if(version<0x50000000 || version>=0x60000000)return Unsupported;
        status.stage=Mapping;
        // PM8150 identification at SID0:0100 and S4 at SID1:1d00;
        // PM8150L identification at SID4:0100 and BOB at SID5:a000.
        const uint16_t ids[EndpointCount]={0x001,0x401,0x11d,0x5a0};
        for(unsigned i=0;i<EndpointCount;++i) {
            status.ppid[i]=ids[i];
            status.apid[i]=find_observer([&](bool o,unsigned at){return table(o,at);},ids[i],status.owner[i]);
            if(status.apid[i]<0)return status.apid[i];
            channelMap[i]=map(0x0e600000+uint64_t(status.apid[i])*0x80,0x80);
            if(!channelMap[i])return -12;
            channel[i]=reinterpret_cast<volatile uint32_t *>(channelMap[i]->getVirtualAddress());
            if(!channel[i])return -12;
        }
        return 0;
    }
    int read(unsigned endpoint,uint8_t reg,uint8_t &value) {
        status.lastEndpoint=endpoint;status.lastReg=reg;
        if(endpoint>=EndpointCount || !channel[endpoint] || poisoned)return Unavailable;
        // Refuse a changed mapping before issuing an observer request.
        uint32_t entry=table(false,0x900+unsigned(status.apid[endpoint])*4);
        if(!entry || ((entry>>8)&0xfff)!=status.ppid[endpoint])return Protocol;
        ++status.reads;
        int error=picasso_spmi_read8(channel[endpoint],reg,value);
        if(error) {
            status.transportError=error;
            if(error==-1){poisoned=true;return -60;}
            return error<0?error:-5;
        }
        return 0;
    }
};
Backend backend;
bool ensureLock() {
    if(__atomic_load_n(&powerLock,__ATOMIC_ACQUIRE))return true;
    IOLock *next=IOLockAlloc();if(!next)return false;
    IOLock *expected=nullptr;
    if(!__atomic_compare_exchange_n(&powerLock,&expected,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(next);
    return true;
}
void report(const char *operation) {
    const Status &s=backend.status;
    IOLog("PICASSO_MIC power op=%s stage=%u error=%d transport=%d qualified=%u vote_owned=0 peripheral_writes=0\n",
        operation,s.stage,s.error,s.transportError,s.qualified);
    for(unsigned i=0;i<2;++i) {
        const Rail &r=s.rail[i];unsigned e=i?BobC:S4A;
        IOLog("PICASSO_MIC rail=%u ppid=%x apid=%d owner=%u pmic=%02x/%02x rev=%u type=%02x/%02x status=%02x mode=%02x en=%02x follow=%02x mv=%u/%u\n",
            i,s.ppid[e],s.apid[e],s.owner[e],s.pmicType[i],s.pmicSubtype[i],r.major,r.type,r.subtype,
            r.status,r.mode,r.enable,r.follow,r.requestedMv,r.validMv);
    }
}
}
extern "C" int picasso_mic_rails_acquire() {
    if(!ensureLock())return -12;
    IOLockLock(powerLock);
    if(backend.poisoned){IOLockUnlock(powerLock);return -60;}
    Status &s=backend.status;bool existing=s.observing!=0;
    s.version=1;s.qualified=0;s.error=0;s.transportError=0;s.ownedVotes=0;
    if(!existing) {
        backend.close();
        for(unsigned i=0;i<EndpointCount;++i)s.apid[i]=-1;
        s.error=backend.open();
    }
    if(!s.error)s.error=qualify(backend,s);
    if(!s.error)s.observing=1;
    else {s.observing=0;backend.close();}
    report("observe");int result=s.error;
    IOLockUnlock(powerLock);return result;
}
extern "C" int picasso_mic_rails_check() {
    IOLock *lock=__atomic_load_n(&powerLock,__ATOMIC_ACQUIRE);
    if(!lock)return Unavailable;
    IOLockLock(lock);Status &s=backend.status;
    if(!s.observing || backend.poisoned){IOLockUnlock(lock);return backend.poisoned?-60:Unavailable;}
    s.transportError=0;int error=qualify(backend,s);
    if(error)report("check");
    IOLockUnlock(lock);return error;
}
extern "C" int picasso_mic_rails_release() {
    IOLock *lock=__atomic_load_n(&powerLock,__ATOMIC_ACQUIRE);
    if(!lock)return 0;
    IOLockLock(lock);
    backend.status.observing=0;backend.status.qualified=0;backend.status.stage=Released;
    backend.close();
    IOLog("PICASSO_MIC power op=release vote_owned=0 peripheral_writes=0 poisoned=%u\n",backend.poisoned);
    IOLockUnlock(lock);return 0;
}
extern "C" void picasso_mic_rails_status(picasso_mic_power::Status *out) {
    if(!out)return;
    IOLock *lock=__atomic_load_n(&powerLock,__ATOMIC_ACQUIRE);
    if(!lock){*out={};return;}
    IOLockLock(lock);*out=backend.status;IOLockUnlock(lock);
}
