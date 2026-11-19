// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board rails from lito.dtsi/lito-regulators.dtsi. RPMh writes use the existing
// APPS DRV2 owner; SPMI is used only to observe regulator readiness.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceMemory.h>
#include "PicassoBluetoothBoard.h"
#include "PicassoSpmi.h"
#include "PicassoMicPower.h"
#include "PicassoRpmh.h"
namespace {
struct Map {IOMemoryMap *keep;volatile uint32_t *va;};
bool map(Map &m,uint64_t pa,unsigned size) {
    if(m.va)return true;
    IODeviceMemory *d=IODeviceMemory::withRange(pa,size);
    m.keep=d?d->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;if(d)d->release();
    m.va=m.keep?reinterpret_cast<volatile uint32_t *>(m.keep->getVirtualAddress()):nullptr;
    return m.va!=nullptr;
}
struct Rail {const char *name;uint16_t ppid;unsigned mv;Map observer;uint32_t address;unsigned baselineMv,baselineMode;bool baselineOn,voted;};
Rail rails[]={{"smpa4",0x11d,1800,{},0,0,0,false,false},
    {"ldoc2",0x541,1304,{},0,0,0,false,false},
    {"ldoc10",0x549,3000,{},0,0,0,false,false},
    {"ldoa14",0x14d,1800,{},0,0,0,false,false}};
Map core,owner,db;
IOLock *lock;
bool initialized,on,poisoned;
uint32_t rd(Map &m,unsigned offset){uint32_t v=m.va[offset/4];__asm__ volatile("dsb sy":::"memory");return v;}
int read(unsigned index,uint8_t reg,uint8_t &value) {
    int e=picasso_spmi_read8(rails[index].observer.va,reg,value);
    if(e){if(e==-1)poisoned=true;return e==-1?-60:-5;}return 0;
}
struct Bus {int read(unsigned i,uint8_t r,uint8_t &v){return ::read(i,r,v);}};
int state(unsigned i,picasso_mic_power::Rail &r) {
    Bus bus;return picasso_mic_power::read_rail(bus,i,r);
}
int write(unsigned i,unsigned offset,unsigned value) {
    int e=0;unsigned retries=0;
    // EBUSY means no request was issued. The shared owner may be bringing
    // ADSP up concurrently; yield with a bounded retry, never reset its TCS.
    do {
        e=picasso_rpmh_active_write(rails[i].address+offset,value);
        if(e!=-16 || retries==20)break;
        ++retries;IOSleep(10);
    } while(true);
    if(e || retries)printf("PICASSO_BT rail-write=%s offset=%u value=%u retries=%u error=%d\n",rails[i].name,offset,value,retries,e);
    if(e==-60)poisoned=true;return e;
}
int initialize() {
    if(initialized)return 0;
    if(!picasso_spmi_init() || !map(core,0xc440000,0x2000) || !map(owner,0xc40a000,0x1000) || !map(db,0x80860000,0x20000))return -6;
    if(rd(core,0)<0x50000000 || rd(core,0)>=0x60000000)return -95;
    uint8_t *snapshot=static_cast<uint8_t *>(IOMallocData(0x20000));if(!snapshot)return -12;
    for(unsigned p=0;p<0x20000;p+=4){uint32_t v=rd(db,p);memcpy(snapshot+p,&v,4);}
    int e=0;
    for(unsigned i=0;i<4 && !e;++i) {
        auto &r=rails[i];uint8_t ee=0;
        int apid=picasso_mic_power::find_observer([&](bool o,unsigned at){return rd(o?owner:core,at);},r.ppid,ee);
        if(apid<0){e=apid;break;}
        if(!map(r.observer,0xe600000+unsigned(apid)*0x80,0x80)){e=-12;break;}
        e=picasso_bt::vrm_address(snapshot,0x20000,r.name,r.address);
        picasso_mic_power::Rail s={};if(!e)e=state(i,s);
        // Unified voltage registers only: reject all unrecognized PMIC blocks.
        if(!e && ((i==0 && (s.type!=3 || s.subtype!=10 || s.major<4)) ||
            (i!=0 && (s.type!=4 || !picasso_bt::unified_ldo(s.subtype) || s.major==0xff))))e=-95;
        r.baselineOn=(s.enable&0x80)!=0;r.baselineMv=s.requestedMv;r.baselineMode=s.mode&7;
        if(!e && (s.follow&15))e=-95;
        printf("PICASSO_BT rail-plan=%s ppid=%x apid=%d addr=%x type=%u/%u rev=%u en=%x status=%x mv=%u/%u error=%d\n",
            r.name,r.ppid,apid,r.address,s.type,s.subtype,s.major,s.enable,s.status,s.requestedMv,s.validMv,e);
    }
    for(unsigned p=0;p<0x20000 && !e;p+=4){uint32_t v=rd(db,p);if(memcmp(snapshot+p,&v,4))e=-37;}
    IOFreeData(snapshot,0x20000);initialized=!e;return e;
}
int retain() {
    // RPMh's aggregate PMIC state does not reveal the inherited APPS DRV2
    // request. Keep the acquired floor until a shared vote owner can release
    // it correctly; the UART pulse still shuts down the Bluetooth controller.
    on=false;
    return 0;
}
}
extern "C" int picasso_bt_power(bool enabled) {
    if(!lock){IOLock *n=IOLockAlloc();if(!n)return -12;IOLock *expected=nullptr;
        if(!__atomic_compare_exchange_n(&lock,&expected,n,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(n);}
    IOLockLock(lock);
    int e=poisoned?-60:initialize();
    if(!e && enabled && !on) {
        for(unsigned i=0;i<4 && !e;++i) {
            auto &r=rails[i];unsigned mv=r.mv>r.baselineMv?r.mv:r.baselineMv;
            // S4 is already shared with capture and must remain at 1.8 V.
            if(i==0 && r.baselineMv!=1800){e=-34;break;}
            if(!picasso_bt::rail_voltage(i,mv)){e=-34;break;}
            // Shared codec IO is already continuously supplied. Observe it
            // without replacing its inherited DRV2 voltage/mode vote.
            if(i==0){
                picasso_mic_power::Rail io={};e=state(i,io);
                if(!e)e=picasso_mic_power::validate(picasso_mic_power::S4A,io);
                continue;
            }
            r.voted=true;
            e=write(i,0,mv);if(!e)e=write(i,8,i==0?6:7);if(!e)e=write(i,4,1);
            if(e)break;
            bool ready=false;
            for(unsigned n=0;n<100;++n) {
                picasso_mic_power::Rail s={};e=state(i,s);if(e)break;
                if((s.status&0xa0)==0x80 && (s.enable&0x80) && s.validMv>=mv && s.validMv<=mv+16){ready=true;break;}
                IOSleep(1);
            }
            if(!e && !ready)e=-60;
        }
        if(!e)on=true;
        else if(!poisoned){(void)retain();printf("PICASSO_BT failed acquisition retained safe vote floors\n");}
    } else if(!e && !enabled)e=retain();
    printf("PICASSO_BT rails enabled=%u on=%u error=%d quarantined=%u retained=%u\n",enabled,on,e,poisoned,rails[1].voted||rails[2].voted||rails[3].voted);
    IOLockUnlock(lock);return e;
}
