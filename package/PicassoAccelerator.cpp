// SPDX-License-Identifier: BSD-2-Clause-Patent
// Empty IOAcceleratorES for the firmware-backed CPU display.
//
// backboardd matches IOAcceleratorES before CoreAnimation composes a frame;
// with no such service the UI never draws, even though the GOP framebuffer
// works. The CPU fallback derives from the original IOAcceleratorES: it
// answers every user-client call with success and zeroed outputs and
// advertises no MetalPluginName, which leaves CoreAnimation on its software
// path. The same approach brought mondrian to the iOS main UI.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>
#include "OriginalAcceleratorABI.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include "PicassoBattery.h"

class PicassoAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PicassoAcceleratorUserClient);
public:
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(PicassoAcceleratorUserClient, IOUserClient);

IOReturn PicassoAcceleratorUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn PicassoAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *) {
    static unsigned calls;
    for (uint32_t i = 0; i < args->scalarOutputCount && i < 16; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    // The first selectors are enough to see what the software path still asks for.
    if (__atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED) < 24) {
        char name[32] = "?";
        proc_selfname(name, sizeof(name));
        IOLog("PICASSO_ACCEL %s selector=%u in=%u out=%u answered by the empty accelerator\n",
            name, selector, args->scalarInputCount, args->scalarOutputCount);
    }
    return kIOReturnSuccess;
}

class PicassoCpuAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(PicassoCpuAccelerator);
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PicassoCpuAccelerator, IOAcceleratorES);

bool PicassoCpuAccelerator::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    setProperty(kIOUserClientClassKey, "PicassoAcceleratorUserClient");
    setProperty("IOAcceleratorStub", kOSBooleanTrue);
    registerService();
    return true;
}

extern "C" IOService *picasso_accelerator_allocate(void) {
    return OSTypeAlloc(PicassoCpuAccelerator);
}

// The gauge is owned by this service's work loop. No measurements or register
// operations run on an interrupt handler or the display's request thread.
class PicassoPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(PicassoPowerSource);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
private:
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    void *gauge=nullptr;
    bool published=false;
    uint64_t lastGoodMs=0,nextReadMs=0;
    unsigned samples=0,failures=0;
    static void tick(OSObject *owner,IOTimerEventSource *);
    void refresh();
    void number(const char *key,unsigned long long value);
    void flag(const char *key,bool value);
    void text(const char *key,const char *value);
    void erase(const char *key);
    void publish(const picasso_battery::Snapshot &s);
    void adapter(const picasso_battery::Snapshot &s);
};
OSDefineMetaClassAndStructors(PicassoPowerSource, IOPMPowerSource);
void PicassoPowerSource::number(const char *key,unsigned long long value){
    const OSSymbol *symbol=OSSymbol::withCString(key);
    OSNumber *n=OSNumber::withNumber(value,32);
    if(symbol && n)setPSProperty(symbol,n);
    OSSafeReleaseNULL(n);OSSafeReleaseNULL(symbol);
}
void PicassoPowerSource::flag(const char *key,bool value){
    const OSSymbol *symbol=OSSymbol::withCString(key);
    if(symbol)setPSProperty(symbol,value?kOSBooleanTrue:kOSBooleanFalse);
    OSSafeReleaseNULL(symbol);
}
void PicassoPowerSource::text(const char *key,const char *value){
    const OSSymbol *symbol=OSSymbol::withCString(key),*string=OSSymbol::withCString(value);
    if(symbol && string)setPSProperty(symbol,const_cast<OSSymbol *>(string));
    OSSafeReleaseNULL(string);OSSafeReleaseNULL(symbol);
}
void PicassoPowerSource::erase(const char *key){
    if(properties->getObject(key)){
        properties->removeObject(key);removeProperty(key);settingsChangedSinceUpdate=true;
    }
}
void PicassoPowerSource::adapter(const picasso_battery::Snapshot &s){
    OSDictionary *details=OSDictionary::withCapacity(3);
    if(!details)return;
    auto put=[&](const char *key,unsigned value){
        OSNumber *n=OSNumber::withNumber(value,32);
        if(n){details->setObject(key,n);n->release();}
    };
    put(kIOPMPSAdapterDetailsFamilyKey,picasso_battery::adapter_family(s.input,s.external));
    if(s.external && s.input.voltageValid)put(kIOPMPSAdapterDetailsVoltage,s.input.millivolts);
    if(s.external && s.input.statusValid && s.capable)
        put(kIOPMPSAdapterDetailsAmperageKey,unsigned(s.input.icl)*50);
    // Effective input limit is not a power brick's rated wattage.
    const OSSymbol *key=OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
    if(key)setPSProperty(key,details);
    OSSafeReleaseNULL(key);details->release();
}
void PicassoPowerSource::publish(const picasso_battery::Snapshot &s){
    setBatteryInstalled(s.present);setExternalConnected(s.external);
    // iOS uses physical input presence to permit percentage increases.
    // ExternalConnected alone updates the charging UI but not this policy.
    flag("AppleRawExternalConnected",s.external);
    setExternalChargeCapable(s.capable);setIsCharging(s.charging);
    setAtWarnLevel(s.warn);setAtCriticalLevel(s.critical);
    flag("PicassoNetCharging",s.netCharging);adapter(s);
    number("PicassoUSBVoltageLimitMV",5000);number("PicassoUSBCurrentLimitMA",3000);
    flag("PicassoUSBInputControlVerified",s.inputControlVerified);
    number("PicassoUSBInputControlError",uint32_t(s.inputControlError));
    number("PicassoUSBTargetMA",s.inputTargetMa);number("PicassoUSBSourceMA",s.inputSourceMa);
    flag("PicassoUSBInputCapVerified",s.inputCapVerified);
    number("PicassoUSBInputCapError",uint32_t(s.inputCapError));
    if(s.inputCapVerified)number("PicassoUSBConfiguredLimitMA",s.inputConfiguredMa);
    else erase("PicassoUSBConfiguredLimitMA");
    if(s.external && s.input.voltageValid)number("PicassoUSBVoltageMV",s.input.millivolts);
    else erase("PicassoUSBVoltageMV");
    if(s.external && s.input.currentValid)number("PicassoUSBCurrentMA",s.input.milliamps);
    else erase("PicassoUSBCurrentMA");
    if(s.external && s.input.statusValid){
        number("PicassoUSBInputLimitMA",unsigned(s.input.icl)*50);
        flag("PicassoInputLimited",(s.input.aicl&0x40)!=0);
    }else{erase("PicassoUSBInputLimitMA");erase("PicassoInputLimited");}
    flag("FullyCharged",s.full);flag("PicassoBatteryDataStale",false);
    flag("CapacityEstimated",true);flag("PicassoCapacityEstimated",true);flag("PicassoTemperatureValid",s.temperatureValid);
    text("PicassoCapacityMethod","default-profile-usable-ocv-current-integration");
    number("PicassoLastSampleMS",s.timeMs);
    erase("ErrorCondition");
    setCurrentCapacity(s.percent);setMaxCapacity(100);
    setVoltage(s.millivolts);setAmperage(s.milliamps);
    setTimeRemaining(s.minutes>=0?s.minutes:0xffff);
    number("InstantAmperage",uint32_t(s.milliamps));
    number("AppleRawCurrentCapacity",s.remainingMah);
    number("AppleRawMaxCapacity",4500);number("DesignCapacity",4500);
    if(s.temperatureValid)number("Temperature",uint32_t(s.temperature*10));
    else erase("Temperature");
    if(s.idValid)number("PicassoBatteryIDOhms",s.idOhm);
    else erase("PicassoBatteryIDOhms");
    // Do not invent adapter wattage, cycle count or learned battery health.
    updateStatus();
    if(!published){registerService();published=true;}
}
bool PicassoPowerSource::start(IOService *provider){
    if(!IOPMPowerSource::start(provider))return false;
    loop=IOWorkLoop::workLoop();
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!loop || !timer || loop->addEventSource(timer)!=kIOReturnSuccess)return false;
    timer->setTimeoutMS(1);return true;
}
void PicassoPowerSource::tick(OSObject *owner,IOTimerEventSource *){
    static_cast<PicassoPowerSource *>(owner)->refresh();
}
void PicassoPowerSource::refresh(){
    if(!gauge)gauge=picasso_battery_open();
    uint64_t tickNs=0;absolutetime_to_nanoseconds(mach_absolute_time(),&tickNs);
    if(gauge && tickNs/1000000<nextReadMs && picasso_battery_changed(gauge)==0){
        timer->setTimeoutMS(500);return;
    }
    picasso_battery::Snapshot s;
    int error=gauge?picasso_battery_read(gauge,&s):-1;
    if(!error){
        lastGoodMs=s.timeMs;nextReadMs=s.timeMs+s.refreshMs;failures=0;publish(s);
        if(samples<12 || samples%12==0){
            // The serial console truncates oversized lines, including newline.
            // Separate records share a sample ID and fit its 256-byte buffer.
            IOLog("PICASSO_BATTERY published sample=%u percent=%d estimated=1 uv=%d ua=%d os_ma=%d temp_dc=%d temp_valid=%u id=%d\n",
                samples,s.percent,s.raw.voltageUv,s.raw.currentUa,s.milliamps,s.temperature,s.temperatureValid,s.idOhm);
            IOLog("PICASSO_BATTERY state sample=%u usb=%02x path=%02x chg=%02x charging=%u full=%u net_charging=%u warn=%u critical=%u\n",
                samples,s.raw.usb,s.raw.path,s.raw.charger,s.charging,s.full,s.netCharging,s.warn,s.critical);
            IOLog("PICASSO_BATTERY input sample=%u input_valid=%u apsd=%02x/%02x icl_ma=%u input_limited=%u usb_mv=%d usb_ma=%d usb_valid=%u/%u\n",
                samples,s.input.statusValid,s.input.apsd,s.input.result,unsigned(s.input.icl)*50,(s.input.aicl&0x40)!=0,
                s.input.millivolts,s.input.milliamps,s.input.voltageValid,s.input.currentValid);
        }
        ++samples;
    }else{
        nextReadMs=0;++failures;uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        if(failures<=3 || failures%12==0)IOLog("PICASSO_BATTERY refresh failed=%d count=%u published=%u\n",error,failures,published);
        if(published && ns/1000000-lastGoodMs>=15000){
            flag("PicassoBatteryDataStale",true);flag("PicassoTemperatureValid",false);
            text("ErrorCondition","BatteryReadFailed");setIsCharging(false);flag("FullyCharged",false);
            setExternalChargeCapable(false);setTimeRemaining(0xffff);
            erase("ExternalConnected");erase("AppleRawExternalConnected");
            erase("AdapterDetails");erase("PicassoNetCharging");
            erase("PicassoUSBVoltageMV");erase("PicassoUSBCurrentMA");
            erase("PicassoUSBInputLimitMA");erase("PicassoInputLimited");
            flag("PicassoUSBInputCapVerified",false);erase("PicassoUSBConfiguredLimitMA");
            flag("PicassoUSBInputControlVerified",false);erase("PicassoUSBTargetMA");erase("PicassoUSBSourceMA");
            setAtWarnLevel(false);setAtCriticalLevel(false);
            erase("Voltage");erase("Amperage");erase("InstantAmperage");erase("Temperature");
            updateStatus();
        }
    }
    // Cable changes are visible within the next light poll; full measurements
    // retain their one/five-second cadence and their original validity checks.
    timer->setTimeoutMS(!error?500:1000);
}
void PicassoPowerSource::stop(IOService *provider){
    if(timer){timer->cancelTimeout();timer->disable();}
    if(loop && timer){loop->removeEventSource(timer);timer->cancelTimeout();}
    IOPMPowerSource::stop(provider);
}
void PicassoPowerSource::free(){
    OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);
    picasso_battery_close(gauge);gauge=nullptr;
    IOPMPowerSource::free();
}
extern "C" bool picasso_power_source_publish(void){
    IOService *platform=IOService::getPlatform();
    PicassoPowerSource *source=OSTypeAlloc(PicassoPowerSource);
    if(!platform || !source || !source->init()){OSSafeReleaseNULL(source);return false;}
    bool attached=source->attach(platform);
    if(attached)source->setName("PicassoPowerSource");
    bool started=attached && source->start(platform);
    if(!started && attached){source->stop(platform);source->detach(platform);}
    OSSafeReleaseNULL(source);
    return started;
}
