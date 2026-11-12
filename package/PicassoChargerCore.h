// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM7250B 5 V input policy. Preserve battery-side limits, AICL,
// termination and hardware thermal protection.
#pragma once
#include <stdint.h>
namespace picasso_charger {
constexpr unsigned maxInputMv=5000,maxInputMa=3000;
// USBIN configuration, from the matching SMB5 controller register interface.
constexpr uint8_t command=0x43,allowOverride=0x44,detectTrigger=0x58,pulses=0x5b;
constexpr uint8_t allowance=0x60,options=0x62,currentLimit=0x70;
struct Result {
    int error=0;
    unsigned writes=0,configuredMa=0;
    uint8_t highVoltageOptions=0,startOnCc=0,allowed=0,overrideValue=0,qcPulses=0;
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
    // Enable BC1.2 explicitly, as required at SMB5 initialization and detach.
    // Disable QC detection/authentication without borrowing USB PHY ownership.
    r.error=masked(bus,options,0x6c,8,r);if(r.error)return r;
    // The matching Type-C initialization starts BC1.2 from VBUS, allowing
    // A-to-C sources to be detected when no CC partner is reported.
    r.error=masked(bus,detectTrigger,0x80,0,r);if(r.error)return r;
    r.error=masked(bus,pulses,0xff,0,r);if(r.error)return r;
    r.error=masked(bus,allowance,0x0f,0,r);if(r.error)return r;
    r.error=masked(bus,allowOverride,0x0f,1,r);if(r.error)return r;
    // Verify all restrictions together. A failed readback is not success.
    r.error=bus.chargerRead(currentLimit,cfg);if(r.error)return r;
    r.error=bus.chargerRead(options,r.highVoltageOptions);if(r.error)return r;
    r.error=bus.chargerRead(detectTrigger,r.startOnCc);if(r.error)return r;
    r.error=bus.chargerRead(allowance,r.allowed);if(r.error)return r;
    r.error=bus.chargerRead(allowOverride,r.overrideValue);if(r.error)return r;
    r.error=bus.chargerRead(pulses,r.qcPulses);if(r.error)return r;
    r.configuredMa=unsigned(cfg)*50;
    r.verified=cfg<=maxInputMa/50 && (r.highVoltageOptions&0x6c)==8 && !(r.startOnCc&0x80) &&
        !(r.allowed&15) && (r.overrideValue&15)==1 && !r.qcPulses;
    if(!r.verified)r.error=-50;
    return r;
}
// Additional observer windows: Type-C and charger thermal status.
struct Source {
    uint8_t sink=0,misc=0,legacy=0,machine=0,thermal=0,die=0,skin=0,connector=0;
    uint8_t suspend=0,aiclOptions=0,mode=0,overrideMode=0,load=0;
    bool valid=false;
};
template<class Bus> int sourceStatus(Bus &bus,Source &out){
    Source next;
    const unsigned ch[]={5,5,5,5,6,6,6,6,1,1,1,1,1};
    const uint8_t reg[]={6,0x0b,0x0d,9,6,7,8,9,0x40,0x80,0x66,0x42,0x65};
    uint8_t *v[]={&next.sink,&next.misc,&next.legacy,&next.machine,&next.thermal,&next.die,&next.skin,&next.connector,
        &next.suspend,&next.aiclOptions,&next.mode,&next.overrideMode,&next.load};
    for(unsigned i=0;i<13;++i){int e=bus.read(ch[i],reg[i],*v[i]);if(e)return e;}
    for(unsigned i=0;i<13;++i){uint8_t again=0;int e=bus.read(ch[i],reg[i],again);if(e)return e;
        if(again!=*v[i])return -55;}
    next.valid=true;out=next;return 0;
}
// A known BC1.2 source may supply VBUS without a detected CC partner.
// Require both Type-C attachment indicators and every Rp/accessory bit to
// agree on absence; water, source mode and VBUS error always reject it.
inline bool noCc(const Source &s){
    return s.valid && !(s.machine&0x20) && !(s.misc&0xd1) &&
        !(s.sink&0x7f) && !(s.legacy&1);
}
inline bool linkSafe(const Source &s){
    return noCc(s) || (s.valid && (s.machine&0x20) && (s.misc&0xd9)==9 &&
        !(s.legacy&1) && !(s.sink&0x71));
}
struct Control {
    unsigned targetMa=0,sourceMa=0;
    int error=0,fallbackError=0;
    bool verified=false;
    // 0 detached, 1 qualifying, 2 source control, 3 safety limit, 4 fault.
    unsigned state=0;
};
struct Controller {
    unsigned candidate=0,stable=0,sourceKey=0;
    uint64_t lastMs=0;
    bool warm=false,owned=false;
    void reset(){candidate=0;stable=0;sourceKey=0;lastMs=0;}
    // On failures, only reduce the input. Never resume a hardware suspension
    // or turn off AICL/JEITA. Readback errors remain visible after fallback.
    template<class Bus> int fallback(Bus &bus,Result &r){
        reset();uint8_t cfg=0;int e=bus.chargerRead(currentLimit,cfg);if(e)return e;
        if(cfg>10){e=masked(bus,currentLimit,0xff,10,r);if(e)return e;}
        e=masked(bus,0x66,7,cfg>=10?2:0,r);if(e)return e;
        e=masked(bus,0x42,1,1,r);if(e)return e;
        e=masked(bus,0x65,0x10,0,r);if(e)return e;
        r.configuredMa=unsigned(cfg>10?10:cfg)*50;owned=false;return 0;
    }
    template<class Sample,class Input> unsigned classify(const Sample &s,const Input &in,const Source &src){
        if(!(s.usb&0x10) || !in.statusValid || !(in.apsd&1) || !src.valid)return 0;
        // A configured SDP remains within USB2's 500 mA ceiling, even if a
        // noncompliant A-to-C cable advertises a stronger pull-up.
        if((in.result&0x7f)==1)return 500;
        if(!linkSafe(src))return 0;
        if(!noCc(src) && !(src.legacy&2)){
            if((src.sink&15)==2)return 3000;
            if((src.sink&15)==4)return 1500;
        }
        switch(in.result&0x7f){
            case 4:return 1500; // BC1.2 CDP
            case 2:case 8:case 0x28:case 0x48:return 1600; // Board DCP default, at 5 V
            default:return 0;
        }
    }
    template<class Bus,class Sample,class Input>
    Control update(Bus &bus,const Sample &s,const Input &in,const Source &src,int temp,bool tempValid,uint64_t now,Result &r){
        Control c;unsigned source=classify(s,in,src);c.sourceMa=source;
        if(tempValid && temp>=400)warm=true;
        else if(tempValid && temp<=380)warm=false;
        bool safe=linkSafe(src) && tempValid && temp>=100 && temp<450 && (s.status&1) &&
            (s.usb&0x1f)==0x10 && (s.path&0x51)==0x11 && !(s.thermal&0x3f) && !(s.chargeError&2) &&
            !(src.suspend&1) && (src.aiclOptions&4) && !(src.thermal&0x79) &&
            !((src.die|src.skin|src.connector)&15) &&
            in.voltageValid && in.millivolts>=4500 && in.millivolts<=5250 && r.verified;
        unsigned target=safe?source:0;
        if(warm && target>500)target=500;
        if(!target || !(s.usb&0x10)){
            c.state=(s.usb&0x10)?3:0;c.targetMa=500;
            c.error=fallback(bus,r);c.verified=!c.error;return c;
        }
        const bool consecutive=lastMs && now>lastMs && now-lastMs<=10000;
        const unsigned key=unsigned(in.result&0x7f) | (unsigned(noCc(src))<<8) |
            (unsigned(src.sink)<<9) | (unsigned(src.legacy)<<17) | (unsigned(src.misc&2)<<24);
        if(target!=candidate || key!=sourceKey || !consecutive){candidate=target;sourceKey=key;stable=1;}
        else if(stable<2)++stable;
        lastMs=now;c.targetMa=target;c.state=2;
        uint8_t cfg=0;c.error=bus.chargerRead(currentLimit,cfg);
        if(c.error){c.state=4;c.fallbackError=fallback(bus,r);return c;}
        // Do not override a pre-existing limit below the normal USB baseline.
        // That can represent another owner or a protection condition.
        if(unsigned(cfg)*50<500 && !owned){c.state=3;c.targetMa=unsigned(cfg)*50;c.verified=true;return c;}
        if(target>500 && stable<2){
            c.state=1;c.targetMa=500;
            if(cfg>10)c.error=masked(bus,currentLimit,0xff,10,r);
            if(!c.error)c.error=masked(bus,0x66,7,2,r);
            if(!c.error)c.error=masked(bus,0x42,1,1,r);
            if(!c.error)c.error=masked(bus,0x65,0x10,0,r);
        }else{
            // Program the ceiling before selecting SMB5 HC or NO_CC mode.
            // NO_CC keeps ICL_OVERRIDE set, matching the vendor mode contract.
            c.error=masked(bus,currentLimit,0xff,uint8_t(target/50),r);
            if(!c.error)c.error=masked(bus,0x66,7,target>500?1:2,r);
            if(!c.error)c.error=masked(bus,0x42,1,target>500 && !noCc(src)?0:1,r);
            if(!c.error)c.error=masked(bus,0x65,0x10,target>500?0x10:0,r);
        }
        if(!c.error){
            uint8_t usb=0,apsd=0,result=0,path=0;
            c.error=bus.read(1,0x10,usb);
            if(!c.error)c.error=bus.read(1,7,apsd);
            if(!c.error)c.error=bus.read(1,8,result);
            if(!c.error)c.error=bus.read(3,0x0b,path);
            if(!c.error && (usb!=s.usb || apsd!=in.apsd || result!=in.result || path!=s.path))c.error=-56;
            Source again;
            if(!c.error)c.error=sourceStatus(bus,again);
            if(!c.error && (again.sink!=src.sink || again.misc!=src.misc || again.legacy!=src.legacy ||
                again.machine!=src.machine || again.thermal!=src.thermal || again.die!=src.die || again.skin!=src.skin ||
                again.connector!=src.connector || again.suspend!=src.suspend || again.aiclOptions!=src.aiclOptions))c.error=-56;
        }
        if(c.error){c.state=4;c.fallbackError=fallback(bus,r);return c;}
        owned=true;r.configuredMa=c.targetMa;c.verified=true;return c;
    }
};

}
