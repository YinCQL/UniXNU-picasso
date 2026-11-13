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
    uint8_t sink=0,misc=0,legacy=0,machine=0,debug=0,otg=0,thermal=0,die=0,skin=0,connector=0;
    uint8_t suspend=0,aiclOptions=0,mode=0,overrideMode=0,load=0;
    bool valid=false;
};
template<class Bus> int sourceStatus(Bus &bus,Source &out){
    Source next;
    const unsigned ch[]={5,5,5,5,5,3,6,6,6,6,1,1,1,1,1};
    const uint8_t reg[]={6,0x0b,0x0d,9,7,0x40,6,7,8,9,0x40,0x80,0x66,0x42,0x65};
    uint8_t *v[]={&next.sink,&next.misc,&next.legacy,&next.machine,&next.debug,&next.otg,&next.thermal,&next.die,&next.skin,&next.connector,
        &next.suspend,&next.aiclOptions,&next.mode,&next.overrideMode,&next.load};
    for(unsigned i=0;i<15;++i){int e=bus.read(ch[i],reg[i],*v[i]);if(e)return e;}
    for(unsigned i=0;i<15;++i){uint8_t again=0;int e=bus.read(ch[i],reg[i],again);if(e)return e;
        if(again!=*v[i])return -55;}
    next.valid=true;out=next;return 0;
}
// Topology is separate from electrical qualification: an A-to-C adapter
// can have completed BC1.2 without a Type-C partner. Check the independent
// debug-accessory status too, rather than interpreting sink==0 by itself.
inline bool noCc(const Source &s){
    return s.valid && !(s.machine&0x20) && !(s.misc&0xc1) &&
        !(s.sink&0x7f) && !(s.debug&1) && !(s.legacy&1);
}
inline bool linkSafe(const Source &s){
    // SMB5 reports exactly one Rp level. Conflicting bits or an attached
    // partner with no Rp must not fall through to a BC1.2 high-current mode.
    const unsigned rp=s.sink&15;
    return (noCc(s) && !(s.misc&0x10)) || (s.valid && (s.machine&0x20) && (s.misc&0xd9)==9 &&
        !(s.legacy&1) && !(s.sink&0xf0) && (rp==2 || rp==4 || rp==8) && !(s.debug&1));
}
// Matching SMB5 selects NO_CC current control for a known BC1.2 source.
// In this narrow debounced, unattached OCP/DCP case, qualify VBUS through
// USBIN fault status, valid path/ADC and protections in update(), rather
// than using the Type-C state machine's VBUS error as a blanket veto.
// Never authorize CDP, SDP, unknown sources, an accessory or a source role
// through this exception, and never write/clear Type-C status registers.
template<class Input> bool legacyVbus(const Input &in,const Source &s){
    return noCc(s) && (s.misc&0xdd)==0x18 && in.statusValid && (in.apsd&1) &&
        ((in.result&0x7f)==2 || (in.result&0x7f)==8);
}
template<class Input> bool sourceLinkSafe(const Input &in,const Source &s){
    return linkSafe(s) || legacyVbus(in,s);
}
struct Control {
    unsigned targetMa=0,sourceMa=0;
    int error=0,fallbackError=0;
    bool verified=false;
    // 0 detached, 1 qualifying, 2 source control, 3 safety limit, 4 fault.
    unsigned state=0;
};
// SMB5 CHGR + 0x42 bit 0 is the battery charging command, independent of
// USBIN's identically numbered input-current override register. Only restore
// a command this instance successfully disabled. Existing disables stay owned
// by their original controller. Hardware JEITA and termination remain active.
struct ThermalControl {
    bool inhibited=false,owned=false,disablePending=false;
    unsigned recovery=0;
    uint64_t lastMs=0;
    int error=0;
    int failed(int value){recovery=0;return error=value;}
    template<class Bus> int update(Bus &bus,bool present,int temp,bool valid,bool fault,uint64_t now){
        bool unsafe=!present || !valid || temp<100 || temp>=450 || fault;
        bool recover=present && valid && temp>=120 && temp<=420 && !fault;
        if(unsafe){inhibited=true;recovery=0;}
        else if(inhibited){
            if(!recover || !lastMs || now<=lastMs || now-lastMs>15000)recovery=0;
            else if(recovery<3)++recovery;
        }
        lastMs=now;error=0;
        uint8_t before=0,after=0;
        error=bus.chargeRead(0x42,before);if(error)return failed(error);
        // An acknowledged disable with a failed readback is confirmed by the
        // next command read. A failed write never authorizes a later enable.
        if(disablePending){owned=!(before&1);disablePending=false;}
        if(inhibited && !(recover && recovery>=3)){
            if(before&1){
                error=bus.chargeWrite(0x42,uint8_t(before&~1));if(error)return failed(error);
                disablePending=true;
                error=bus.chargeRead(0x42,after);if(error)return failed(error);
                disablePending=false;
                if(after&1)return failed(-57);
                owned=true;
            }
        }else if(inhibited){
            if(owned && !(before&1)){
                // Recheck hardware protection immediately before restoring.
                uint8_t chargeError=0,thermal=0;
                error=bus.chargeRead(0x07,chargeError);if(error)return failed(error);
                error=bus.chargeRead(0x0d,thermal);if(error)return failed(error);
                if((chargeError&2) || (thermal&0x3f)){recovery=0;return 0;}
                error=bus.chargeWrite(0x42,uint8_t(before|1));if(error)return failed(error);
                error=bus.chargeRead(0x42,after);if(error)return failed(error);
                if(!(after&1))return failed(-57);
            }
            inhibited=false;owned=false;recovery=0;
        }
        return 0;
    }
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
        if(!sourceLinkSafe(in,src))return 0;
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
        bool safe=sourceLinkSafe(in,src) && !(src.otg&1) && tempValid && temp>=100 && temp<450 && (s.status&1) &&
            (s.usb&0x3f)==0x10 && (s.path&0x51)==0x11 && !(s.thermal&0x3f) && !(s.chargeError&2) &&
            !(src.suspend&1) && (src.aiclOptions&4) && !(src.thermal&0x79) &&
            !((src.die|src.skin|src.connector)&15) &&
            (!legacyVbus(in,src) || (in.currentValid && in.milliamps>=0 && in.milliamps<=int(maxInputMa))) &&
            in.voltageValid && in.millivolts>=4500 && in.millivolts<=5250 && r.verified;
        unsigned target=safe?source:0;
        if(warm && target>500)target=500;
        if(!target || !(s.usb&0x10)){
            c.state=(s.usb&0x10)?3:0;c.targetMa=500;
            c.error=fallback(bus,r);c.verified=!c.error;return c;
        }
        const bool consecutive=lastMs && now>lastMs && now-lastMs<=10000;
        const unsigned key=unsigned(in.result&0x7f) | (unsigned(noCc(src))<<8) |
            (unsigned(src.sink)<<9) | (unsigned(src.legacy)<<17) | (unsigned(src.misc&2)<<24) | (unsigned(src.misc&0x10)<<23);
        if(target!=candidate || key!=sourceKey || !consecutive){candidate=target;sourceKey=key;stable=1;}
        else if(stable<2)++stable;
        lastMs=now;c.targetMa=target;c.state=2;
        uint8_t cfg=0;c.error=bus.chargerRead(currentLimit,cfg);
        if(c.error){c.state=4;c.fallbackError=fallback(bus,r);return c;}
        // Do not override a pre-existing limit below the normal USB baseline.
        // That can represent another owner or a protection condition.
        if(unsigned(cfg)*50<500){
            // This controller never programs below 500 mA. A later lower
            // setting revokes our ownership just as an inherited one does.
            reset();owned=false;c.state=3;c.targetMa=unsigned(cfg)*50;
            r.configuredMa=c.targetMa;c.verified=true;return c;
        }
        if(target>500 && stable<2){
            c.state=1;c.targetMa=500;
            if(cfg>10)c.error=masked(bus,currentLimit,0xff,10,r);
            if(!c.error)c.error=masked(bus,0x66,7,2,r);
            if(!c.error)c.error=masked(bus,0x42,1,1,r);
            if(!c.error)c.error=masked(bus,0x65,0x10,0,r);
        }else{
            // Program the ceiling before selecting SMB5 HC or NO_CC mode.
            // NO_CC keeps ICL_OVERRIDE set, matching the vendor mode contract.
            // A NO_CC source with the Type-C indication rises in 250 mA
            // steps only after two stable full samples. New faults revoke it.
            unsigned programmed=target;
            if(legacyVbus(in,src) && target>500 && unsigned(cfg)*50+250<programmed)
                programmed=unsigned(cfg)*50+250;
            c.targetMa=programmed;
            c.error=masked(bus,currentLimit,0xff,uint8_t(programmed/50),r);
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
                again.machine!=src.machine || again.debug!=src.debug || again.otg!=src.otg || again.thermal!=src.thermal || again.die!=src.die || again.skin!=src.skin ||
                again.connector!=src.connector || again.suspend!=src.suspend || again.aiclOptions!=src.aiclOptions))c.error=-56;
        }
        // SMB5 reruns AICL after changing NO_CC current mode. The command
        // is self-clearing: I/O acceptance is not proof of settled current.
        // ADC/settled ICL in later snapshots supply that observation.
        if(!c.error && noCc(src) && c.targetMa>500 && c.targetMa!=unsigned(cfg)*50){
            uint8_t path=0,cmd=0;
            c.error=bus.read(3,0x0b,path);
            if(!c.error && (path!=s.path || (path&0x51)!=0x11))c.error=-56;
            if(!c.error)c.error=bus.read(6,0x44,cmd);
            if(!c.error)c.error=bus.miscWrite(0x44,uint8_t(cmd|1));
            if(!c.error)++r.writes;
        }
        if(c.error){c.state=4;c.fallbackError=fallback(bus,r);return c;}
        owned=true;r.configuredMa=c.targetMa;c.verified=true;return c;
    }
};

}
