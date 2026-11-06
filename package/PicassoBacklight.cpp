// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso g7a panel: PM8150L SID5 WLED5, not Sheng's I2C backlight.
// Only adopt a running, fault-free stock configuration; gate the light on
// OS display power/brightness requests. Panel scanout, rails and CABC remain
// owned by firmware. This does not implement deep sleep.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include "PicassoButtonsCore.h"
#include "PicassoBacklightCore.h"
extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void picasso_brightness_static_init() __asm__("__GLOBAL__sub_I_PicassoBrightness.cpp");
IOService *picasso_brightness_allocate();
}
namespace {
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t base, uint64_t size) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    ~Window() { OSSafeReleaseNULL(map); }
};
struct Channel {
    volatile uint32_t *va;
    void write(unsigned offset, uint32_t value) { va[offset/4]=value; __asm__ volatile("dsb sy" ::: "memory"); }
    uint32_t read(unsigned offset) { uint32_t value=va[offset/4]; __asm__ volatile("dsb sy" ::: "memory"); return value; }
    void delay() { IODelay(1); }
};
// Mapped only once at display startup, published with release/acquire. The
// mutex serializes all requests and survives until shutdown with these maps.
struct Backend {
    Window observer[2], writer[2];
    IOService *display = nullptr;
    IOLock *lock = nullptr;
    picasso_backlight::State state;
    unsigned reports = 0, errorReports = 0;
    bool lastSink = false, lastWrite = false;
    uint8_t lastReg = 0, lastValue = 0;
    int lastError = 0;
    void report_error(int error) {
        if (errorReports >= 16) return;
        ++errorReports;
        IOLog("PICASSO_WLED rejected error=%d fatal=%u stage=%u gate=%02x expected=%02x fault=%02x flash=%02x source=%02x expected_source=%02x level=%u expected_level=%u sync=%02x last_write=%u last_sink=%u last_reg=%02x last_value=%02x transport=%d\n",
            error,state.failed,state.stage,state.gate,uint8_t((state.module&0x7f)|(state.on?0x80:0)),
            state.faults,state.flash,state.currentSource,state.source,state.observedLevel,state.level,state.sync,
            lastWrite,lastSink,lastReg,lastValue,lastError);
    }
    bool adopting = true;
    bool displayOn = true;
    uint32_t requested = 0;
    int read(bool sink, uint8_t reg, uint8_t &value) {
        Channel channel{observer[sink].va};
        int error = picasso_buttons::read8(channel, reg, value);
        lastWrite=false;lastSink=sink;lastReg=reg;lastValue=value;lastError=error;
        if (error || adopting) IOLog("PICASSO_WLED read sink=%u reg=%02x value=%02x error=%d\n",sink,reg,value,error);
        return error;
    }
    int write(bool sink, uint8_t reg, uint8_t value) {
        lastWrite=true;lastSink=sink;lastReg=reg;lastValue=value;lastError=-1;
        Channel channel{writer[sink].va};
        channel.write(0x10,value);
        channel.write(0,uint32_t(reg)<<4); // v5 EXT_WRITEL, 1 byte
        for (unsigned n=0;n<1000;++n) {
            uint32_t status=channel.read(8);
            if (status & 1) {lastError=int(status & 0xe);return lastError;}
            channel.delay();
        }
        return -1;
    }
    void soft_start() { IOSleep(10); }
};
Backend *backend;
}
extern "C" void picasso_backlight_start(IOService *display) {
    unsigned enabled=0;
    PE_parse_boot_argn("picasso-wled",&enabled,sizeof(enabled));
    if (enabled!=1 || !display || backend) return;
    Window core, owners;
    if (!core.open(0xc440000,0x2000) || !owners.open(0xc40a000,0x1000)) {
        IOLog("PICASSO_WLED refused MMIO mapping\n"); return;
    }
    uint32_t version=core.va[0];
    if (!picasso_buttons::version_supported(version)) {
        IOLog("PICASSO_WLED refused version=%08x\n",version); return;
    }
    auto read=[&](bool owner,uint32_t offset) {return (owner?owners.va:core.va)[offset/4];};
    int apid[2]={picasso_backlight::find_apid(read,picasso_backlight::control_ppid),
                 picasso_backlight::find_apid(read,picasso_backlight::sink_ppid)};
    if (apid[0]<0 || apid[1]<0) {
        IOLog("PICASSO_WLED refused owner control=%d sink=%d\n",apid[0],apid[1]); return;
    }
    Backend *next=new Backend;
    if (!next) return;
    next->lock=IOLockAlloc();
    bool mapped=next->lock!=nullptr;
    for(unsigned i=0;i<2 && mapped;++i)
        mapped=next->observer[i].open(0xe600000+unsigned(apid[i])*0x80,0x80) &&
               next->writer[i].open(0xc600000+unsigned(apid[i])*0x10000,0x1000);
    int error=mapped?next->state.adopt(*next):-4;
    IOLog("PICASSO_WLED adopt version=%08x control_apid=%d sink_apid=%d source=%u error=%d\n",version,apid[0],apid[1],next->state.source,error);
    if(error) {if(next->lock)IOLockFree(next->lock);delete next;return;}
    next->adopting=false;
    next->requested=next->state.level;
    display->retain();
    next->display=display;
    __atomic_store_n(&backend,next,__ATOMIC_RELEASE);
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle) {IOLog("PICASSO_BRIGHTNESS preModLoad failed\n");return;}
    picasso_brightness_static_init();
    int status=OSMetaClass::postModLoad(handle);
    if(status) {IOLog("PICASSO_BRIGHTNESS metaclass error=%x\n",status);return;}
    IOService *service=picasso_brightness_allocate();
    bool initialized=service && service->init();
    bool attached=initialized && service->attach(display);
    bool started=attached && service->start(display);
    if(!started && attached) {service->stop(display);service->detach(display);}
    OSSafeReleaseNULL(service);
    IOLog("PICASSO_BRIGHTNESS started=%u\n",started);
}
extern "C" IOReturn picasso_backlight_power(IOService *provider, bool on) {
    Backend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    if(!b)return kIOReturnNotReady;
    if(provider!=b->display)return kIOReturnNotFound;
    IOLockLock(b->lock);
    int error=0;
    if(on && b->requested)error=b->state.brightness(*b,picasso_backlight::clamp(b->requested));
    if(!error)error=b->state.set(*b,on && b->requested!=0);
    if(!error)b->displayOn=on;
    if(b->reports<64) {++b->reports;IOLog("PICASSO_WLED power=%u error=%d verified=%u\n",on,error,!error); }
    if(error)b->report_error(error);
    IOReturn result=!error?kIOReturnSuccess:b->state.failed?kIOReturnIOError:kIOReturnBusy;
    IOLockUnlock(b->lock);
    return result;
}
extern "C" IOReturn picasso_backlight_level(uint32_t level) {
    Backend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    if(!b)return kIOReturnNotReady;
    if(level>picasso_backlight::max_level)return kIOReturnBadArgument;
    IOLockLock(b->lock);
    level=picasso_backlight::clamp(level);
    int error=b->state.failed?picasso_backlight::configuration:0;
    if(!error && b->displayOn) {
        if(level)error=b->state.brightness(*b,level);
        if(!error)error=b->state.set(*b,level!=0);
    }
    if(!error)b->requested=level;
    if(b->reports<128 || (b->reports&63)==0) {IOLog("PICASSO_WLED brightness=%u applied=%u deferred=%u error=%d\n",
        level,b->state.on?b->state.level:0,!b->displayOn,error);}
    ++b->reports;
    if(error)b->report_error(error);
    IOReturn result=!error?kIOReturnSuccess:b->state.failed?kIOReturnIOError:kIOReturnBusy;
    IOLockUnlock(b->lock);
    return result;
}
extern "C" bool picasso_backlight_current(uint32_t *level) {
    Backend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    if(!b || !level)return false;
    IOLockLock(b->lock);bool ok=!b->state.failed;
    if(ok)*level=b->requested;
    IOLockUnlock(b->lock);return ok;
}
