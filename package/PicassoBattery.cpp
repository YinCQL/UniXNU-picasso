// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM7250B QG/SMB5 measurement backend; the charger policy remains untouched.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include "PicassoButtonsCore.h"
#include "PicassoSpmi.h"
#include "PicassoBattery.h"
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
struct Backend {
    Window channels[5],adcWriter;
    uint8_t subtype=0;
    bool adcReady=false,ponRead=false;
    picasso_battery::Estimator estimator;
    int read(unsigned channel,uint8_t reg,uint8_t &value){return picasso_spmi_read8(channels[channel].va,reg,value);}
    int write(uint8_t reg,uint8_t value){return picasso_spmi_write8(adcWriter.va,reg,value);}
    void wait(){IOSleep(10);}
    int pon(){
        uint8_t a=0,b=0,c=0,d=0;
        if(read(0,0x70,a)||read(0,0x71,b)||read(0,0x70,c)||read(0,0x71,d)||a!=c||b!=d)return 0;
        return picasso_battery::voltage_uv(uint16_t(a|unsigned(b)<<8));
    }
};
}
extern "C" void *picasso_battery_open(){
    if(!picasso_spmi_init())return nullptr;
    Window core,owners;
    if(!core.open(0xc440000,0x2000)||!owners.open(0xc40a000,0x1000))return nullptr;
    if(!picasso_buttons::version_supported(core.va[0]))return nullptr;
    auto lookup=[&](bool owner,uint32_t offset){return (owner?owners.va:core.va)[offset/4];};
    const unsigned ppid[5]={0x248,0x213,0x210,0x211,0x231};
    int apid[5]={-1,-1,-1,-1,-1};Backend *b=new Backend;
    if(!b)return nullptr;
    for(unsigned i=0;i<5;++i){
        apid[i]=picasso_buttons::find_apid(lookup,ppid[i]);
        bool ok=apid[i]>=0 && b->channels[i].open(0xe600000+unsigned(apid[i])*0x80,0x80);
        if(!ok && i<4){delete b;return nullptr;}
        if(i==4)b->adcReady=ok && b->adcWriter.open(0xc600000+unsigned(apid[i])*0x10000,0x1000);
    }
    uint8_t type=0;int error=b->read(0,4,type);
    if(!error)error=b->read(0,5,b->subtype);
    IOLog("PICASSO_BATTERY probe qg=%d usb=%d charger=%d path=%d adc=%d type=%02x subtype=%02x error=%d\n",
        apid[0],apid[1],apid[2],apid[3],apid[4],type,b->subtype,error);
    if(error || type!=0x0d || (b->subtype!=3 && b->subtype!=4)){delete b;return nullptr;}
    return b;
}
extern "C" void picasso_battery_close(void *context){delete static_cast<Backend *>(context);}
extern "C" int picasso_battery_read(void *context,picasso_battery::Snapshot *out){
    Backend *b=static_cast<Backend *>(context);if(!b || !out)return -1;
    picasso_battery::Sample sample;
    int error=picasso_battery::sample(*b,b->subtype,sample);
    if(error){b->estimator.gap();return error;}
    uint16_t tempRaw=0,idRaw=0;int temp=-10000,id=0;
    if(b->adcReady && (sample.status&1)){
        if(!picasso_battery::adc_read(*b,0x4a,tempRaw))temp=picasso_battery::temperature_deci_c(tempRaw);
        if(!picasso_battery::adc_read(*b,0x4b,idRaw))id=picasso_battery::battery_id_ohm(idRaw);
    }
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    int pon=0;
    if(!b->ponRead && (sample.status&1)){pon=b->pon();b->ponRead=true;}
    picasso_battery::Snapshot next;
    b->estimator.update(sample,ns/1000000,temp,temp!=-10000,pon,next);
    next.idOhm=id;next.idValid=id>0;
    *out=next;return 0;
}
