// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_BLUETOOTH_BOARD_H
#define PICASSO_BLUETOOTH_BOARD_H
#include <stdint.h>
#include <stddef.h>
namespace picasso_bt {
constexpr uint64_t Uart=0x894000,Wrapper=0x8c0000,Gcc=0x100000,Smmu=0x15000000;
constexpr unsigned Irq=638,Sid=0x4e3,Rx=41,SwCtrl=73;
constexpr uint64_t pin_address(unsigned gpio){return 0x0f900000ULL+gpio*0x1000ULL;}
struct Clock {uint32_t hz,m,n,cfg;};
// PMIC5 510 LDO register layout (qcom_spmi-regulator.c v6.12).
inline bool unified_ldo(unsigned subtype) {
    return (subtype>=0x6a && subtype<=0x6c) || (subtype>=0x71 && subtype<=0x73) ||
        (subtype==0x7a || subtype==0x7b || subtype==0x7d);
}
inline bool rail_voltage(unsigned rail,unsigned mv) {
    return rail==0?mv==1800:rail==1?(mv>=1304 && mv<=1328):
        rail==2?(mv>=3000 && mv<=3312):rail==3?(mv>=1800 && mv<=1984):false;
}
inline bool clock(unsigned baud,Clock &c) {
    // Stock gcc-lito table: GPLL0 even = 300 MHz, dual-edge M/N.
    if(baud==2400){c={19200000,0,0,1};return true;}
    if(baud==115200){c={7372800,384,15625,0x2601};return true;}
    if(baud==3000000){c={96000000,8,25,0x2601};return true;}
    return false;
}
inline unsigned divider(unsigned baud,unsigned hz,unsigned sample) {
    if(!baud || (sample!=16 && sample!=32))return 0;
    uint64_t target=uint64_t(baud)*sample,div=(hz+target/2)/target;
    if(!div || div>4095)return 0;
    uint64_t actual=hz/div,diff=actual>target?actual-target:target-actual;
    return diff*100<=target?unsigned(div):0;
}
inline int vrm_address(const uint8_t *db,unsigned size,const char *name,uint32_t &address) {
    auto u16=[](const uint8_t *p){return unsigned(p[0])|(unsigned(p[1])<<8);};
    auto u32=[&](const uint8_t *p){return uint32_t(u16(p))|(uint32_t(u16(p+2))<<16);};
    if(size<144 || u32(db+4)!=0x0c0330db)return -22;
    uint8_t key[8]={};unsigned n=0;while(n<8 && name[n]){key[n]=name[n];++n;}
    if(!n || n==8)return -22;
    unsigned found=0;
    for(unsigned i=0;i<8;++i) {
        const uint8_t *slave=db+8+i*16;
        if(u16(slave)!=4)continue;
        unsigned at=144+u16(slave+2),count=u16(slave+6);
        if(at>size || count>(size-at)/24)return -22;
        for(unsigned j=0;j<count;++j) {
            const uint8_t *e=db+at+j*24;bool equal=true;
            for(unsigned k=0;k<8;++k)equal&=key[k]==e[k];
            if(!equal)continue;
            address=u32(e+16);
            if(++found!=1 || (address&3) || ((address>>16)&7)!=4)return -22;
        }
    }
    return found==1?0:-2;
}
}
extern "C" void picasso_bluetooth_probe();
extern "C" int picasso_bt_power(bool);
extern "C" int picasso_bt_firmware_read(const char *,const uint8_t **,size_t *);
extern "C" void picasso_bt_firmware_release(const uint8_t *,size_t);
#endif
