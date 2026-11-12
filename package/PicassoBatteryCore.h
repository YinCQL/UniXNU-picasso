// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM7250B QG last ADC and SMB5 status. The SOC register is software-written;
// keep it as a raw observation, not a live capacity estimate.
#pragma once
#include <stdint.h>
namespace picasso_battery {
struct Sample {
    uint8_t status=0,soc=0,usb=0,charger=0;
    uint16_t voltageRaw=0,currentRaw=0;
    int voltageUv=0,currentUa=0;
};
inline int voltage_uv(uint16_t raw){return int(uint64_t(raw)*194637/1000);}
inline int current_ua(uint16_t raw,uint8_t subtype){
    int value=(raw&0x8000)?int(raw)-65536:int(raw);
    return int(int64_t(value)*(subtype==3?152588:305176)/1000);
}
template<class Bus> int sample(Bus &bus,uint8_t subtype,Sample &out){
    if(subtype!=3 && subtype!=4)return -20;
    Sample next;
    uint8_t raw[4]={},again[4]={};
    int error=0;
    if((error=bus.read(0,0x08,next.status)) || (error=bus.read(0,0xbf,next.soc)) ||
       (error=bus.read(1,0x10,next.usb)) || (error=bus.read(2,0x06,next.charger)))return error;
    // Re-read the complete ADC tuple. No latch, FIFO acknowledgement or
    // configuration register is written by this observer.
    bool stable=false;
    for(unsigned attempt=0;attempt<3 && !stable;++attempt){
        for(unsigned i=0;i<4;++i)if((error=bus.read(0,uint8_t(0xc0+i),raw[i])))return error;
        for(unsigned i=0;i<4;++i)if((error=bus.read(0,uint8_t(0xc0+i),again[i])))return error;
        stable=true;for(unsigned i=0;i<4;++i)if(raw[i]!=again[i])stable=false;
    }
    if(!stable)return -21;
    next.voltageRaw=uint16_t(raw[0]|unsigned(raw[1])<<8);
    next.currentRaw=uint16_t(raw[2]|unsigned(raw[3])<<8);
    next.voltageUv=voltage_uv(next.voltageRaw);
    next.currentUa=current_ua(next.currentRaw,subtype);
    out=next;
    if(!(next.status&0x80) || !(next.status&1) || next.voltageUv<2000000 || next.voltageUv>5000000)return -22;
    return 0;
}
}
