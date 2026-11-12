// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "PicassoBatteryProfile.h"
namespace picasso_battery {
struct Sample {
    uint8_t status=0,soc=0,usb=0,charger=0,path=0,thermal=0,chargeError=0;
    uint16_t voltageRaw=0,currentRaw=0;
    int voltageUv=0,currentUa=0;
};
inline int voltage_uv(uint16_t raw){return int(uint64_t(raw)*194637/1000);}
// QG is positive on discharge. IOPMPowerSource uses the opposite sign.
inline int current_ua(uint16_t raw,uint8_t subtype){
    int value=(raw&0x8000)?int(raw)-65536:int(raw);
    return int(int64_t(value)*(subtype==3?152588:305176)/1000);
}
template<class Bus> int sample(Bus &bus,uint8_t subtype,Sample &out){
    if(subtype!=3 && subtype!=4)return -20;
    for(unsigned attempt=0;attempt<3;++attempt){
        Sample next;uint8_t raw[4]={},again[4]={},usb=0,path=0,status=0,charger=0,thermal=0,chargeError=0;
        int error=0;
        if((error=bus.read(0,0x08,next.status)) || (error=bus.read(0,0xbf,next.soc)) ||
           (error=bus.read(1,0x10,next.usb)) || (error=bus.read(2,0x06,next.charger)) ||
           (error=bus.read(2,0x07,next.chargeError)) || (error=bus.read(2,0x0d,next.thermal)) ||
           (error=bus.read(3,0x0b,next.path)))return error;
        for(unsigned i=0;i<4;++i)if((error=bus.read(0,uint8_t(0xc0+i),raw[i])))return error;
        for(unsigned i=0;i<4;++i)if((error=bus.read(0,uint8_t(0xc0+i),again[i])))return error;
        if((error=bus.read(0,8,status)) || (error=bus.read(1,0x10,usb)) ||
           (error=bus.read(2,6,charger)) || (error=bus.read(3,0x0b,path)) ||
           (error=bus.read(2,7,chargeError)) || (error=bus.read(2,0x0d,thermal)))return error;
        bool stable=status==next.status && usb==next.usb && path==next.path && charger==next.charger &&
            thermal==next.thermal && chargeError==next.chargeError;
        for(unsigned i=0;i<4;++i)if(raw[i]!=again[i])stable=false;
        if(!stable)continue;
        next.voltageRaw=uint16_t(raw[0]|unsigned(raw[1])<<8);
        next.currentRaw=uint16_t(raw[2]|unsigned(raw[3])<<8);
        next.voltageUv=voltage_uv(next.voltageRaw);
        next.currentUa=current_ua(next.currentRaw,subtype);
        if(!(next.status&0x80))return -22;
        if((next.status&1) && (next.voltageUv<2000000 || next.voltageUv>5000000))return -22;
        out=next;return 0;
    }
    return -21;
}
inline int interpolate(int x,int a,int b,int ya,int yb){
    return a==b?ya:ya+int(int64_t(x-a)*(yb-ya)/(b-a));
}
inline int temperature_deci_c(uint16_t code){
    int mv=int(uint32_t(code)*1875/0x70e4);
    if(code==0 || code>=0x70e4 || mv>thermistor[0][0] || mv<thermistor[69][0])return -10000;
    for(unsigned i=1;i<70;++i)if(mv>=thermistor[i][0])
        return interpolate(mv,thermistor[i-1][0],thermistor[i][0],thermistor[i-1][1],thermistor[i][1]);
    return -10000;
}
inline int battery_id_ohm(uint16_t code){
    if(!code || code>=0x70e4)return 0;
    return int(uint64_t(code)*100000/(0x70e4-code));
}
inline int ocv_soc_column(int uv,unsigned col){
    for(unsigned row=0;row<sizeof(profileSoc)/sizeof(profileSoc[0]);++row){
        int voltage=profileOcv[row][col]*100;
        if(uv>=voltage){
            if(!row)return 10000;
            return interpolate(uv,profileOcv[row-1][col]*100,voltage,
                profileSoc[row-1],profileSoc[row]);
        }
    }
    return 0;
}
inline int ocv_soc(int uv,int temperature){
    if(temperature<=profileTemperature[0]*10)return ocv_soc_column(uv,0);
    if(temperature>=profileTemperature[5]*10)return ocv_soc_column(uv,5);
    unsigned col=0;
    while(col<4 && temperature>profileTemperature[col+1]*10)++col;
    // Invert each voltage curve before interpolating SOC across temperature.
    return interpolate(temperature,profileTemperature[col]*10,profileTemperature[col+1]*10,
        ocv_soc_column(uv,col),ocv_soc_column(uv,col+1));
}
inline int usable_ocv_soc(int uv,int temperature){
    // Board cutoff is 3.4 V. The default charging curve reaches 100% below
    // the 4.4 V float limit at every temperature; the boot model caps that
    // endpoint at 99%. Keep hundredth-percent precision for both endpoints.
    const int empty=ocv_soc(profileCutoffUv,temperature);
    const int raw=ocv_soc(uv,temperature);
    if(raw<=empty)return 0;
    if(raw>=profileFullSoc)return 10000;
    return int(int64_t(raw-empty)*10000/(profileFullSoc-empty));
}
// APSD and settled input limits are reported by SMB5, not negotiated here.
struct Input {
    uint8_t apsd=0,result=0,icl=0,aicl=0;
    int millivolts=0,milliamps=0;
    bool statusValid=false,voltageValid=false,currentValid=false;
};
inline uint32_t adapter_family(const Input &s,bool external){
    if(!external)return 0;
    if(!s.statusValid || !(s.apsd&1))return 0xe0004007u;
    switch(s.result&0x7f){
        case 1:return 0xe0004000u;
        case 4:return 0xe0004005u;
        case 2:case 8:case 0x28:case 0x48:return 0xe0004004u;
        default:return 0xe0004007u;
    }
}
template<class Bus> int input_status(Bus &bus,Input &out){
    Input next;uint8_t again[4]={};
    const unsigned channel[4]={1,1,3,3};
    const uint8_t reg[4]={7,8,8,0x0a};
    uint8_t *first[4]={&next.apsd,&next.result,&next.icl,&next.aicl};
    for(unsigned i=0;i<4;++i){int e=bus.read(channel[i],reg[i],*first[i]);if(e)return e;}
    for(unsigned i=0;i<4;++i){int e=bus.read(channel[i],reg[i],again[i]);if(e)return e;
        if(again[i]!=*first[i])return -35;}
    if(next.icl>100)return -36;
    next.statusValid=true;out=next;return 0;
}
inline int adc_voltage_uv(uint16_t code,unsigned scale){
    return int((uint64_t(code)*1875000/0x70e4)*scale);
}
struct Snapshot {
    Sample raw;
    Input input;
    uint64_t timeMs=0;
    int percent=0,remainingMah=0,millivolts=0,milliamps=0,temperature=0,idOhm=0,minutes=-1;
    bool present=false,external=false,capable=false,charging=false,full=false;
    bool temperatureValid=false,idValid=false,estimated=true;
    bool netCharging=false,warn=false,critical=false;
    unsigned refreshMs=5000;
    bool inputCapVerified=false;
    int inputCapError=0,inputConfiguredMa=0;
};
// Board low-voltage thresholds and 50 mV recovery hysteresis. Require fresh
// observations across three seconds before escalating a voltage dip. The OS
// receives standard warning/critical properties and owns orderly shutdown.
struct LowBattery {
    uint64_t previousMs=0,lowSince=0,criticalSince=0;
    bool low=false,critical=false;
    void reset(){previousMs=0;lowSince=0;criticalSince=0;low=false;critical=false;}
    void update(Snapshot &s){
        s.warn=false;s.critical=false;s.refreshMs=5000;
        if(!s.present){reset();return;}
        if(!previousMs || s.timeMs<=previousMs || s.timeMs-previousMs>15000){lowSince=0;criticalSince=0;}
        previousMs=s.timeMs;
        const int lowUv=s.temperatureValid && s.temperature<0?3800000:3500000;
        const int emptyUv=s.temperatureValid && s.temperature<0?3000000:3100000;
        const int uv=s.raw.voltageUv;
        // A valid USB path can still supply less than the system consumes.
        // Clear low-battery state only on meaningful net charge, not cable
        // presence or a near-zero current inside the gauge's rest band.
        // A cell below the board's empty threshold still needs protection.
        if(s.capable && s.raw.currentUa<-45000 && uv>=emptyUv){reset();return;}
        if(uv<lowUv){if(!lowSince)lowSince=s.timeMs;if(s.timeMs-lowSince>=3000)low=true;}
        else {lowSince=0;if(uv>lowUv+50000)low=false;}
        const bool nearEmpty=(s.percent<=2 && uv<3380000) || uv<emptyUv;
        if(nearEmpty){if(!criticalSince)criticalSince=s.timeMs;if(s.timeMs-criticalSince>=3000)critical=true;}
        else {criticalSince=0;if(uv>3430000)critical=false;}
        s.warn=s.percent<=20 || low || critical;s.critical=critical;
        if(uv<lowUv || low || s.percent<=2)s.refreshMs=1000;
    }
};
// 4500 mAh is nominal, not a learned health measurement. Integrate only
// adjacent observations; an unread interval is never filled with old current.
struct Estimator {
    bool ready=false;
    uint64_t lastMs=0,restMs=0,correctionMs=0;
    int lastCurrent=0,restVoltage=0;
    int64_t remainingUaMs=0;
    LowBattery lowBattery;
    static constexpr int64_t fullUaMs=4500LL*1000*3600000;
    void gap(){lastMs=0;restMs=0;correctionMs=0;lowBattery.reset();}
    void update(const Sample &s,uint64_t now,int temp,bool tempValid,int ponUv,Snapshot &out){
        out=Snapshot{};out.raw=s;out.timeMs=now;out.present=(s.status&1)!=0;
        out.external=(s.usb&0x10)!=0;
        out.capable=out.external && (s.path&0x11)==0x11 && !(s.path&0x40) && !(s.usb&0x0f);
        out.millivolts=s.voltageUv/1000;out.milliamps=-s.currentUa/1000;
        out.temperature=temp;out.temperatureValid=tempValid;
        unsigned mode=s.charger&7;
        bool fault=(s.thermal&0x0f) || (s.chargeError&2);
        // The charger state describes its active charging cycle. A system load
        // can temporarily draw more than the input supplies; preserve that net
        // discharge in Amperage and capacity integration without hiding the cycle.
        out.charging=out.present && out.capable && !fault && mode>=1 && mode<=4;
        out.netCharging=out.present && s.currentUa<0;
        out.full=out.present && out.capable && !fault && mode==5 && s.voltageUv>=4300000 && s.currentUa>=-45000 && s.currentUa<=45000;
        if(!out.present){ready=false;gap();return;}
        uint64_t dt=lastMs && now>lastMs && now-lastMs<=15000?now-lastMs:0;
        if(!ready){
            // S7 is a boot measurement, used once. Reject implausible values;
            // live voltage is an explicitly approximate initialization fallback.
            int initial=ponUv>=3000000 && ponUv<=4500000?ponUv:s.voltageUv;
            remainingUaMs=fullUaMs*usable_ocv_soc(initial,tempValid?temp:250)/10000;
            ready=true;
        }else if(dt){
            remainingUaMs-=int64_t(lastCurrent+s.currentUa)*int64_t(dt)/2;
        }
        if(out.full)remainingUaMs=fullUaMs;
        // A resting cell can slowly correct integration drift. Load-induced
        // voltage dips must not make the reported capacity jump down and up.
        if(dt && s.currentUa>=-45000 && s.currentUa<=45000){
            if(!restMs){restMs=now;restVoltage=s.voltageUv;correctionMs=now;}
            restVoltage=(restVoltage*7+s.voltageUv)/8;
            if(now-restMs>=120000 && now-correctionMs>=60000 && tempValid){
                int64_t target=fullUaMs*usable_ocv_soc(restVoltage,temp)/10000;
                int64_t change=target-remainingUaMs,limit=fullUaMs/100;
                if(change>limit)change=limit;if(change<-limit)change=-limit;
                remainingUaMs+=change;correctionMs=now;
            }
        }else{restMs=0;correctionMs=0;}
        if(remainingUaMs<0)remainingUaMs=0;
        if(remainingUaMs>fullUaMs)remainingUaMs=fullUaMs;
        out.percent=int((remainingUaMs*100+fullUaMs/2)/fullUaMs);
        if(!out.full && out.percent==100)out.percent=99;
        out.remainingMah=int(remainingUaMs/3600000000LL);
        if(!out.capable && s.currentUa>45000)
            out.minutes=int(remainingUaMs/(int64_t(s.currentUa)*60000));
        // Charging time depends on taper and negotiated limits; leave unknown.
        lowBattery.update(out);
        lastMs=now;lastCurrent=s.currentUa;
    }
};
// ADC5 application conversion, separate from QG/charger configuration. One
// owner calls this under its work-loop gate; all bus transactions share SPMI.
template<class Bus> int adc_read(Bus &bus,uint8_t channel,uint16_t &out){
    const bool thermal=channel==0x4a || channel==0x4b;
    if(!thermal && channel!=7 && channel!=8)return -30;
    uint8_t status=0;int error=bus.read(4,8,status);
    if(error)return error;if(status&2)return -31;
    uint8_t cfg[5]={};
    for(unsigned i=0;i<5;++i)if((error=bus.read(4,uint8_t(0x42+i),cfg[i])))return error;
    // Thermistor/ID use ratiometric calibration; USB uses absolute calibration.
    cfg[0]=uint8_t((cfg[0]&~0x7c)|(thermal?0x18:0x28));
    cfg[1]&=uint8_t(~7);cfg[2]=channel;cfg[3]=uint8_t((cfg[3]&~15)|(thermal?2:0));cfg[4]|=0x80;
    for(unsigned i=0;i<5;++i)if((error=bus.write(uint8_t(0x42+i),cfg[i])))return error;
    if((error=bus.write(0x47,0x80)))return error;
    for(unsigned i=0;i<10;++i){
        bus.wait();
        if((error=bus.read(4,8,status)))return error;
        if((status&3)!=1)continue;
        uint8_t lo=0,hi=0,selected=0;
        if((error=bus.read(4,0x44,selected)) || (error=bus.read(4,0x50,lo)) || (error=bus.read(4,0x51,hi)))return error;
        if(selected!=channel)return -32;
        uint16_t code=uint16_t(lo|unsigned(hi)<<8);
        if((thermal && !code) || code>=0x70e4)return -33;
        out=code;return 0;
    }
    return -34;
}
}
