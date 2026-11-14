// SPDX-License-Identifier: BSD-2-Clause-Patent
// Final XNU halt callback for Picasso, after filesystems and PM notifications.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "PicassoButtonsCore.h"
#include "PicassoSpmi.h"
#include "PicassoPowerCore.h"
namespace {
struct Window {
    IOMemoryMap *map=nullptr;
    volatile uint32_t *va=nullptr;
    bool open(uint64_t base,uint64_t size) {
        IODeviceMemory *memory=IODeviceMemory::withRange(base,size);
        map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);
        va=map?reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()):nullptr;
        return va!=nullptr;
    }
    ~Window(){OSSafeReleaseNULL(map);}
};
struct Reply { int32_t status; uint32_t value; };
// The vendor atomic SMC ABI resumes with the returned register state. Queries
// at startup only discover support; power operations run only on kPEHaltCPU.
static Reply secure(uint32_t function,uint64_t argument,uint32_t info=1,uint64_t argument2=0) {
    register uint64_t x0 __asm__("x0")=function;
    register uint64_t x1 __asm__("x1")=info;
    register uint64_t x2 __asm__("x2")=argument;
    register uint64_t x3 __asm__("x3")=argument2;
    register uint64_t x4 __asm__("x4")=0;
    register uint64_t x5 __asm__("x5")=0;
    register uint64_t x6 __asm__("x6")=0;
    register uint64_t x7 __asm__("x7")=0;
    for(unsigned attempt=0;attempt<64;++attempt) {
        __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
            "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) ::
            "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
        if(uint32_t(x0)!=1) return {int32_t(x0),uint32_t(x1)};
    }
    return {-23,0};
}
static bool available(uint32_t function) {
    Reply r=secure(0xc2000601,function);
    return r.status==0 && r.value==1;
}
struct Backend {
    Window observer,writer,hold;
    bool stopArbiter=false,secureHold=false,clearDownload=false,disableDebug=false;
    unsigned started=0;
    int (*previous)(unsigned)=nullptr;
    int read(uint8_t reg,uint8_t &value){return picasso_spmi_read8(observer.va,reg,value);}
    int write(uint8_t reg,uint8_t value){return picasso_spmi_write8(writer.va,reg,value);}
    void delay(unsigned us){IODelay(us);}
    int prepare(){
        // Match the board's normal shutdown path. These controls are changed
        // only after filesystem teardown, never during startup or a panic.
        if(clearDownload){
            Reply r=secure(0xc2000110,0,2,0);
            IOLog("PICASSO_POWER clear-download status=%d result=%u\n",r.status,r.value);
            if(r.status)return r.status;
        }
        if(disableDebug){
            Reply r=secure(0xc2000109,1,2,0);
            IOLog("PICASSO_POWER disable-debug status=%d result=%u\n",r.status,r.value);
            if(r.status)return r.status;
        }
        return 0;
    }
    int quiesce(){return picasso_spmi_quiesce();}
    int halt_arbiter(){
        if(!stopArbiter)return 0;
        Reply r=secure(0xc2000901,0);
        IOLog("PICASSO_POWER arbiter status=%d result=%u\n",r.status,r.value);
        return r.status;
    }
    void drop_hold(){
        IOLog("PICASSO_POWER drop-hold pmic=shutdown secure=%u pa=0c264000\n",secureHold);
        if(secureHold){
            Reply r=secure(0xc2000902,0);
            IOLog("PICASSO_POWER secure-hold returned status=%d result=%u; MMIO fallback\n",r.status,r.value);
        }
        __asm__ volatile("dsb sy" ::: "memory");
        hold.va[0]=0;
        __asm__ volatile("dsb sy" ::: "memory");
    }
};
Backend *backend;
static int halt(unsigned type) {
    Backend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    if(!b)return -1;
    if(type!=kPEHaltCPU) return b->previous?b->previous(type):-1;
    if(__atomic_exchange_n(&b->started,1U,__ATOMIC_ACQ_REL)) return -1;
    IOLog("PICASSO_POWER halt begin type=%u\n",type);
    picasso_power::Result result=picasso_power::shutdown(*b);
    IOLog("PICASSO_POWER halt failed stage=%u error=%d ctl=%02x enable=%02x\n",
        result.stage,result.error,result.control,result.enabled);
    return result.error;
}
}
extern "C" void picasso_power_start(void) {
    if(backend || !picasso_spmi_init())return;
    Window core,owners;
    if(!core.open(0xc440000,0x2000)||!owners.open(0xc40a000,0x1000))return;
    uint32_t version=core.va[0];
    if(!picasso_buttons::version_supported(version))return;
    auto lookup=[&](bool owner,uint32_t offset){return (owner?owners.va:core.va)[offset/4];};
    int apid=picasso_buttons::find_apid(lookup,picasso_buttons::pon_ppid);
    if(apid<0){IOLog("PICASSO_POWER refused PON owner\n");return;}
    Backend *next=new Backend;
    if(!next)return;
    bool mapped=next->observer.open(0xe600000+unsigned(apid)*0x80,0x80) &&
        next->writer.open(0xc600000+unsigned(apid)*0x10000,0x1000) && next->hold.open(0xc264000,0x1000);
    uint8_t subtype=0;
    int error=mapped?next->read(5,subtype):-4;
    if(error || subtype!=4){IOLog("PICASSO_POWER refused mapping=%u subtype=%02x error=%d\n",mapped,subtype,error);delete next;return;}
    next->stopArbiter=available(0x02000901);
    next->secureHold=available(0x02000902);
    next->clearDownload=available(0x02000110);
    next->disableDebug=available(0x02000109);
    next->previous=PE_halt_restart;
    __atomic_store_n(&backend,next,__ATOMIC_RELEASE);
    PE_halt_restart=halt;
    IOLog("PICASSO_POWER ready apid=%d subtype=%02x arbiter=%u secure_hold=%u dload=%u debug=%u previous=%u\n",
        apid,subtype,next->stopArbiter,next->secureHold,next->clearDownload,next->disableDebug,next->previous!=nullptr);
}
