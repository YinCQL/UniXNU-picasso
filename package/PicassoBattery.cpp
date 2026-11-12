// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM7250B measurements and source-qualified 5 V input control.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include "PicassoButtonsCore.h"
#include "PicassoSpmi.h"
#include "PicassoBattery.h"
#include "PicassoChargerCore.h"
extern "C" int picasso_usb_charger_detect(unsigned);
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
    Window channels[7],adcWriter,usbWriter;
    bool chargerReady=false,chargerReported=false;
    int lastChargerError=0,lastControlError=0;
    unsigned lastConfiguredMa=0,lastControlState=99,lastSourceMa=0;
    picasso_charger::Controller controller;
    picasso_charger::Control control;
    uint64_t lastDetect=0;
    unsigned detectAttempts=0;
    bool adapterHistory=false;
    int peakInputMa=0,bestBatteryMa=0,startPercent=0,endPercent=0;
    uint64_t adapterStart=0,adapterEnd=0;
    picasso_charger::Result charger;
    uint8_t subtype=0;
    bool adcReady=false,ponRead=false;
    picasso_battery::Estimator estimator;
    int read(unsigned channel,uint8_t reg,uint8_t &value){return picasso_spmi_read8(channels[channel].va,reg,value);}
    int write(uint8_t reg,uint8_t value){return picasso_spmi_write8(adcWriter.va,reg,value);}
    int chargerRead(uint8_t reg,uint8_t &value){return read(1,reg,value);}
    int chargerWrite(uint8_t reg,uint8_t value){return picasso_spmi_write8(usbWriter.va,reg,value);}
    void restrictInput(){
        charger=chargerReady?picasso_charger::restrictInput(*this):picasso_charger::Result{};
        if(!chargerReady)charger.error=-51;
    }
    void failed(){
        control=picasso_charger::Control{};control.state=4;
        control.error=controller.fallback(*this,charger);
    }
    void detect(const picasso_battery::Sample &s,const picasso_battery::Input &in,uint64_t now){
        if(!(s.usb&0x10)){lastDetect=0;detectAttempts=0;return;}
        // SDP/unknown may be stale after switching a cable without a sampled
        // unplug. The USB owner rejects requests while a host is configured.
        if(in.statusValid && (in.apsd&1) && in.result!=0 && in.result!=1 && in.result!=0x10)return;
        if((lastDetect && now-lastDetect<3000) || detectAttempts>=3)return;
        lastDetect=now;
        int error=picasso_usb_charger_detect(1);if(error)return;
        ++detectAttempts;
        error=controller.fallback(*this,charger);
        if(!error)error=picasso_charger::masked(*this,0x62,8,8,charger);
        if(!error)error=chargerWrite(0x41,1);
        // APSD completion from before the command cannot authorize a new source.
        // Discard this read's classification; the next timer observes it afresh.
        if(!error)IOSleep(1500);
        int restore=picasso_usb_charger_detect(0);
        if(!error)error=restore;
        IOLog("PICASSO_CHARGER detection attempt=%u error=%d restore=%d\n",detectAttempts,error,restore);
    }
    void report(const picasso_charger::Source &src){
        if(!chargerReported || charger.error!=lastChargerError || charger.writes || charger.configuredMa!=lastConfiguredMa ||
            control.state!=lastControlState || control.sourceMa!=lastSourceMa || control.error!=lastControlError){
            IOLog("PICASSO_CHARGER cap_mv=%u cap_ma=%u cfg_ma=%u verified=%u error=%d writes=%u mode=source-control\n",
                picasso_charger::maxInputMv,picasso_charger::maxInputMa,charger.configuredMa,charger.verified,charger.error,charger.writes);
            IOLog("PICASSO_CHARGER source_ma=%u target_ma=%u state=%u ok=%u error=%d fallback=%d cc=%02x/%02x/%02x thermal=%02x/%02x/%02x/%02x aicl=%02x\n",
                control.sourceMa,control.targetMa,control.state,control.verified,control.error,control.fallbackError,
                src.sink,src.misc,src.legacy,src.thermal,src.die,src.skin,src.connector,src.aiclOptions);
            chargerReported=true;lastChargerError=charger.error;lastConfiguredMa=charger.configuredMa;
            lastControlState=control.state;lastSourceMa=control.sourceMa;lastControlError=control.error;
        }
    }
    void history(const picasso_battery::Snapshot &s){
        if(control.sourceMa>500 && s.input.currentValid){
            if(!adapterHistory){adapterHistory=true;adapterStart=s.timeMs;startPercent=s.percent;
                peakInputMa=0;bestBatteryMa=s.milliamps;}
            if(s.input.milliamps>peakInputMa)peakInputMa=s.input.milliamps;
            if(s.milliamps>bestBatteryMa)bestBatteryMa=s.milliamps;
            endPercent=s.percent;adapterEnd=s.timeMs;
        }else if(adapterHistory && s.input.statusValid && s.input.result==1){
            IOLog("PICASSO_CHARGER previous_adapter seconds=%llu peak_usb_ma=%d best_battery_ma=%d percent=%d>%d\n",
                (adapterEnd-adapterStart)/1000,peakInputMa,bestBatteryMa,startPercent,endPercent);
            adapterHistory=false;
        }
    }
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
    const unsigned ppid[7]={0x248,0x213,0x210,0x211,0x231,0x215,0x216};
    int apid[7]={-1,-1,-1,-1,-1,-1,-1};Backend *b=new Backend;
    if(!b)return nullptr;
    for(unsigned i=0;i<7;++i){
        apid[i]=picasso_buttons::find_apid(lookup,ppid[i]);
        bool ok=apid[i]>=0 && b->channels[i].open(0xe600000+unsigned(apid[i])*0x80,0x80);
        if(!ok && i<4){delete b;return nullptr;}
        if(i==1)b->chargerReady=ok && b->usbWriter.open(0xc600000+unsigned(apid[i])*0x10000,0x1000);
        if(i==4)b->adcReady=ok && b->adcWriter.open(0xc600000+unsigned(apid[i])*0x10000,0x1000);
    }
    uint8_t type=0;int error=b->read(0,4,type);
    if(!error)error=b->read(0,5,b->subtype);
    IOLog("PICASSO_BATTERY probe qg=%d usb=%d charger=%d path=%d adc=%d type=%02x subtype=%02x error=%d\n",
        apid[0],apid[1],apid[2],apid[3],apid[4],type,b->subtype,error);
    if(error || type!=0x0d || (b->subtype!=3 && b->subtype!=4)){delete b;return nullptr;}
    b->restrictInput();
    return b;
}
extern "C" void picasso_battery_close(void *context){delete static_cast<Backend *>(context);}
extern "C" int picasso_battery_read(void *context,picasso_battery::Snapshot *out){
    Backend *b=static_cast<Backend *>(context);if(!b || !out)return -1;
    b->restrictInput();
    picasso_battery::Sample sample;
    int error=picasso_battery::sample(*b,b->subtype,sample);
    if(error){b->failed();b->estimator.gap();return error;}
    picasso_battery::Input input;
    if(sample.usb&0x10)picasso_battery::input_status(*b,input);
    uint16_t tempRaw=0,idRaw=0;int temp=-10000,id=0;
    if(b->adcReady && (sample.status&1)){
        if(!picasso_battery::adc_read(*b,0x4a,tempRaw))temp=picasso_battery::temperature_deci_c(tempRaw);
        if(!picasso_battery::adc_read(*b,0x4b,idRaw))id=picasso_battery::battery_id_ohm(idRaw);
    }
    if(b->adcReady && (sample.usb&0x10) && (sample.path&0x11)==0x11){
        uint16_t code=0;
        if(!picasso_battery::adc_read(*b,8,code)){
            input.millivolts=picasso_battery::adc_voltage_uv(code,16)/1000;
            input.voltageValid=input.millivolts>=2500 && input.millivolts<=22000;
        }
        if(!picasso_battery::adc_read(*b,7,code)){
            // PM7250B USB sense is 0.2 V/A in buck mode.
            input.milliamps=picasso_battery::adc_voltage_uv(code,5)/1000;
            input.currentValid=input.milliamps<=5000;
        }
    }
    // Do not combine ADC conversions from before an unplug with a new source.
    uint8_t usb=0,path=0;
    error=b->read(1,0x10,usb);if(!error)error=b->read(3,0x0b,path);
    if(error || usb!=sample.usb || path!=sample.path){b->failed();b->estimator.gap();return error?error:-37;}
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    const uint64_t now=ns/1000000;
    picasso_charger::Source source;
    picasso_charger::sourceStatus(*b,source);
    b->control=b->controller.update(*b,sample,input,source,temp,temp!=-10000,now,b->charger);
    b->report(source);
    int pon=0;
    if(!b->ponRead && (sample.status&1)){pon=b->pon();b->ponRead=true;}
    picasso_battery::Snapshot next;
    b->estimator.update(sample,ns/1000000,temp,temp!=-10000,pon,next);
    next.idOhm=id;next.idValid=id>0;next.input=input;
    next.inputCapVerified=b->charger.verified;next.inputCapError=b->charger.error;
    next.inputConfiguredMa=int(b->charger.configuredMa);
    next.inputControlVerified=b->control.verified;
    next.inputControlError=b->control.error;next.inputTargetMa=int(b->control.targetMa);
    next.inputSourceMa=int(b->control.sourceMa);
    if(next.external)next.refreshMs=1000;
    b->history(next);
    b->detect(sample,input,now);
    *out=next;return 0;
}
