// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM7250B input restrictions. The existing charger owns source selection,
// AICL, battery float/current, termination and thermal protection.
#pragma once
#include <stdint.h>
namespace picasso_charger {
constexpr unsigned maxInputMv=5000,maxInputMa=3000;
// USBIN configuration, from the matching SMB5 controller register interface.
constexpr uint8_t command=0x43,allowOverride=0x44,pulses=0x5b;
constexpr uint8_t allowance=0x60,options=0x62,currentLimit=0x70;
struct Result {
    int error=0;
    unsigned writes=0,configuredMa=0;
    uint8_t highVoltageOptions=0,allowed=0,overrideValue=0,qcPulses=0;
    bool verified=false;
};
// Keep unrelated bits and every lower current limit. In particular, this
// does not select high-current override or resume a suspended USB input.
template<class Bus> int masked(Bus &bus,uint8_t reg,uint8_t mask,uint8_t value,Result &r){
    uint8_t before=0,after=0;
    int error=bus.chargerRead(reg,before);if(error)return error;
    const uint8_t next=uint8_t((before&~mask)|(value&mask));
    if(next!=before){
        error=bus.chargerWrite(reg,next);if(error)return error;
        ++r.writes;
    }
    error=bus.chargerRead(reg,after);if(error)return error;
    return (after&mask)==(next&mask)?0:-50;
}
template<class Bus> Result restrictInput(Bus &bus){
    Result r;uint8_t cfg=0,apsd=0;
    // Restrict the programmed high-current ceiling before voltage changes.
    // Automatic USB/Type-C source levels top out at 3 A without QC/PD voltage
    // negotiation; leave their selection/override registers untouched.
    r.error=bus.chargerRead(currentLimit,cfg);if(r.error)return r;
    if(cfg>maxInputMa/50){
        r.error=masked(bus,currentLimit,0xff,uint8_t(maxInputMa/50),r);
        if(r.error)return r;
    }
    r.error=bus.chargerRead(8,apsd);if(r.error)return r;
    // Stop autonomous pulses before requesting 5 V from an inherited QC link.
    r.error=masked(bus,options,0x20,0,r);if(r.error)return r;
    if(apsd&0x60){
        uint8_t voltage=0;r.error=bus.chargerRead(9,voltage);if(r.error)return r;
        if(voltage&6){
            r.error=bus.chargerWrite(command,8);if(r.error)return r;
            ++r.writes;
        }
    }
    // Disable QC detection/authentication, keeping BC1.2 enabled as inherited.
    r.error=masked(bus,options,0x64,0,r);if(r.error)return r;
    r.error=masked(bus,pulses,0xff,0,r);if(r.error)return r;
    r.error=masked(bus,allowance,0x0f,0,r);if(r.error)return r;
    r.error=masked(bus,allowOverride,0x0f,1,r);if(r.error)return r;
    // Verify all restrictions together. A failed readback is not success.
    r.error=bus.chargerRead(currentLimit,cfg);if(r.error)return r;
    r.error=bus.chargerRead(options,r.highVoltageOptions);if(r.error)return r;
    r.error=bus.chargerRead(allowance,r.allowed);if(r.error)return r;
    r.error=bus.chargerRead(allowOverride,r.overrideValue);if(r.error)return r;
    r.error=bus.chargerRead(pulses,r.qcPulses);if(r.error)return r;
    r.configuredMa=unsigned(cfg)*50;
    r.verified=cfg<=maxInputMa/50 && !(r.highVoltageOptions&0x64) &&
        !(r.allowed&15) && (r.overrideValue&15)==1 && !r.qcPulses;
    if(!r.verified)r.error=-50;
    return r;
}
}
