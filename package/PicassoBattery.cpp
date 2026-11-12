// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only first-stage QG/SMB5 observations. No charge-control or SOC writes.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include "PicassoButtonsCore.h"
#include "PicassoSpmi.h"
#include "PicassoBatteryCore.h"
namespace {
struct Window {
    IOMemoryMap *map=nullptr;
    volatile uint32_t *va=nullptr;
    bool open(uint64_t base,uint64_t size){
        IODeviceMemory *memory=IODeviceMemory::withRange(base,size);
        map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);
        va=map?reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()):nullptr;
        return va!=nullptr;
    }
    ~Window(){OSSafeReleaseNULL(map);}
};
struct Bus {
    Window channels[3];
    int read(unsigned channel,uint8_t reg,uint8_t &value){return picasso_spmi_read8(channels[channel].va,reg,value);}
};
void observe(void *,wait_result_t){
    thread_set_thread_name(current_thread(),"picasso-battery");
    if(!picasso_spmi_init())return;
    Window core,owners;
    if(!core.open(0xc440000,0x2000)||!owners.open(0xc40a000,0x1000))return;
    if(!picasso_buttons::version_supported(core.va[0]))return;
    auto lookup=[&](bool owner,uint32_t offset){return (owner?owners.va:core.va)[offset/4];};
    const unsigned ppid[3]={0x248,0x213,0x210};
    int apid[3];Bus bus;
    for(unsigned i=0;i<3;++i){
        apid[i]=picasso_buttons::find_apid(lookup,ppid[i]);
        if(apid[i]<0 || !bus.channels[i].open(0xe600000+unsigned(apid[i])*0x80,0x80)){
            IOLog("PICASSO_BATTERY unavailable ppid=%03x apid=%d\n",ppid[i],apid[i]);return;
        }
    }
    uint8_t type=0,subtype=0;int error=bus.read(0,4,type);
    if(!error)error=bus.read(0,5,subtype);
    IOLog("PICASSO_BATTERY probe qg=%d usb=%d charger=%d type=%02x subtype=%02x error=%d\n",
        apid[0],apid[1],apid[2],type,subtype,error);
    if(error || type!=0x0d || (subtype!=3 && subtype!=4))return;
    // Bounded five-minute observation window at five seconds per sample.
    // It does not replace the existing OS power-source publication until
    // the voltage/current path and the separate SOC estimator are qualified.
    for(unsigned n=0;n<60;++n){
        picasso_battery::Sample sample;
        error=picasso_battery::sample(bus,subtype,sample);
        IOLog("PICASSO_BATTERY sample=%u error=%d status=%02x vraw=%04x iraw=%04x uv=%d ua=%d usb=%02x chg=%02x soc_raw=%u soc_live=0\n",
            n,error,sample.status,sample.voltageRaw,sample.currentRaw,sample.voltageUv,sample.currentUa,
            sample.usb,sample.charger,sample.soc);
        if(error && error!=-21 && error!=-22)return;
        IOSleep(5000);
    }
}
}
extern "C" void picasso_battery_observe_start(void){
    static bool started;
    if(started)return;
    thread_t thread=nullptr;
    if(kernel_thread_start(observe,nullptr,&thread)==KERN_SUCCESS){started=true;thread_deallocate(thread);}
}
