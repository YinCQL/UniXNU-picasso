// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_MIC_POWER_H
#define PICASSO_MIC_POWER_H
#include <stdint.h>
// The WCD supply check observes shared PM8150 S4 and PM8150L BOB rails.
// It does not acquire a VRM vote or change a regulator register. Callers must
// periodically recheck the observation and stop capture if it becomes invalid.
namespace picasso_mic_power {
enum Endpoint { PmicA, PmicC, S4A, BobC, EndpointCount };
enum Stage { Idle, Controller, Mapping, PmicIdentity, S4Reading, BobReading, Qualified, Released };
enum Error { Invalid=-22, Unsupported=-95, Unavailable=-6, NotReady=-11,
    Voltage=-34, Unstable=-37, Protocol=-71 };
struct Rail {
    uint8_t major,type,subtype,status,mode,enable,follow;
    uint16_t requestedMv,validMv;
};
struct Status {
    uint32_t version,stage,checks,reads,qualified,observing,ownedVotes;
    int32_t error,transportError;
    uint32_t lastEndpoint,lastReg;
    uint16_t ppid[EndpointCount];
    int32_t apid[EndpointCount];
    uint8_t owner[EndpointCount],pmicType[2],pmicSubtype[2];
    Rail rail[2],comparison[2];
    uint32_t mismatch[2];
};
// Stock SPMI v5 uses an observer channel even when a different EE owns writes.
// Prefer the APPS-owned mapping, as the stock PPID table builder does. Reject
// two APPS mappings or two equally qualified observer mappings as ambiguous.
template<class Read> int find_observer(Read read,uint16_t ppid,uint8_t &owner) {
    int chosen=-1;unsigned candidates=0;bool apps=false;
    for(unsigned i=0;i<512;++i) {
        uint32_t map=read(false,0x900+i*4);
        if(!map || ((map>>8)&0xfff)!=ppid)continue;
        uint8_t ee=uint8_t(read(true,0x700+i*4)&7);
        if(ee==0) {
            if(apps)return Protocol;
            chosen=int(i);owner=ee;apps=true;candidates=1;
        } else if(!apps) {
            chosen=int(i);owner=ee;++candidates;
        }
    }
    return chosen<0?Unavailable:(!apps && candidates!=1)?Protocol:chosen;
}
// The unified regulator readiness API consumes bit 7. Bit 5 is latched OCP;
// other status bits may change while both observations remain ready.
constexpr uint8_t RailStatusMask=0xa0;
inline bool same(const Rail &a,const Rail &b) {
    return a.major==b.major && a.type==b.type && a.subtype==b.subtype &&
        !((a.status^b.status)&RailStatusMask) && a.mode==b.mode && a.enable==b.enable &&
        a.follow==b.follow && a.requestedMv==b.requestedMv && a.validMv==b.validMv;
}
template<class Bus> int read_rail(Bus &bus,unsigned endpoint,Rail &out) {
    Rail r={};uint8_t lo=0,hi=0;
    int e=bus.read(endpoint,1,r.major);
    if(!e)e=bus.read(endpoint,4,r.type);
    if(!e)e=bus.read(endpoint,5,r.subtype);
    if(!e)e=bus.read(endpoint,8,r.status);
    if(!e)e=bus.read(endpoint,0x45,r.mode);
    if(!e)e=bus.read(endpoint,0x46,r.enable);
    if(!e)e=bus.read(endpoint,0x47,r.follow);
    if(!e)e=bus.read(endpoint,0x40,lo);
    if(!e)e=bus.read(endpoint,0x41,hi);
    if(!e)r.requestedMv=uint16_t(lo|(uint16_t(hi)<<8));
    // On a unified regulator with READY set, these are the accepted voltage
    // setting. Reading only the requested setting would miss a pending step.
    if(!e)e=bus.read(endpoint,0x42,lo);
    if(!e)e=bus.read(endpoint,0x43,hi);
    if(!e)r.validMv=uint16_t(lo|(uint16_t(hi)<<8));
    out=r;return e;
}
inline int validate(unsigned endpoint,const Rail &r) {
    if(endpoint==S4A) {
        // PMIC5 HFSMPS shares the unified 1 mV layout from major revision 4.
        if(r.type!=3 || r.subtype!=0x0a || r.major<4 || r.major==0xff)return Unsupported;
    } else if(endpoint==BobC) {
        // The board has PM8150L BOB510_CONFIG (subtype 5, revision 0).
        // Its unified voltage registers use mV, independently of the older
        // BOB_CONFIG revision numbering. Do not accept the old PMIC4 block.
        if(r.type!=41 || r.subtype!=5 || r.major!=0)return Unsupported;
    } else return Invalid;
    if((r.status&RailStatusMask)!=0x80 || !(r.enable&0x80) || (r.follow&0x0f))return NotReady;
    unsigned mode=r.mode&7;
    // PMIC5 PFM/AUTO/PWM are 4/6/7. Retention and BOB pass-through do not
    // qualify a regulated voltage for the codec, even if a request is cached.
    if(mode!=4 && mode!=6 && mode!=7)return NotReady;
    if(endpoint==S4A)
        return r.requestedMv==1800 && r.validMv==1800?0:Voltage;
    // Stock lito audio votes 3296 mV on shared bobc1. RPMh takes the maximum
    // of consumer requests; the board limits this rail to 3960 mV. A codec
    // vote therefore does not imply that the aggregate must equal 3296 mV.
    // Check both the requested and accepted settings within that envelope.
    // READY plus repeated matching configuration qualify the observed setting;
    // do not infer a DAC step or an analog measurement from their difference.
    return r.requestedMv>=3296 && r.requestedMv<=3960 &&
        r.validMv>=3296 && r.validMv<=3960?0:Voltage;
}
template<class Bus> int qualify(Bus &bus,Status &s) {
    s.qualified=0;s.error=0;s.stage=PmicIdentity;++s.checks;
    s.mismatch[0]=s.mismatch[1]=0;
    for(unsigned i=0;i<2;++i){s.pmicType[i]=s.pmicSubtype[i]=0;s.rail[i]=s.comparison[i]={};}
    const uint8_t subtype[2]={0x1e,0x1f};
    for(unsigned i=0;i<2;++i) {
        uint8_t type=0,part=0,againType=0,againPart=0;
        int e=bus.read(i,4,type);
        if(!e)e=bus.read(i,5,part);
        if(!e)e=bus.read(i,4,againType);
        if(!e)e=bus.read(i,5,againPart);
        s.pmicType[i]=type;s.pmicSubtype[i]=part;
        if(e)return s.error=e;
        if(type!=againType || part!=againPart)return s.error=Unstable;
        if(type!=0x51 || part!=subtype[i])return s.error=Unsupported;
    }
    for(unsigned i=0;i<2;++i) {
        s.stage=i?BobReading:S4Reading;
        Rail again={};unsigned endpoint=i?BobC:S4A;
        int e=read_rail(bus,endpoint,s.rail[i]);
        if(!e)e=read_rail(bus,endpoint,again);
        if(e)return s.error=e;
        s.comparison[i]=again;
        const Rail &first=s.rail[i];
        s.mismatch[i]=(first.major!=again.major?1U:0U)|(first.type!=again.type?2U:0U)|
            (first.subtype!=again.subtype?4U:0U)|(((first.status^again.status)&RailStatusMask)?8U:0U)|
            (first.mode!=again.mode?16U:0U)|(first.enable!=again.enable?32U:0U)|
            (first.follow!=again.follow?64U:0U)|(first.requestedMv!=again.requestedMv?128U:0U)|
            (first.validMv!=again.validMv?256U:0U);
        if(!same(first,again))return s.error=Unstable;
        e=validate(endpoint,s.rail[i]);if(e)return s.error=e;
    }
    s.stage=Qualified;s.qualified=1;return 0;
}
}
extern "C" int picasso_mic_rails_acquire();
extern "C" int picasso_mic_rails_check();
extern "C" int picasso_mic_rails_release();
extern "C" void picasso_mic_rails_status(picasso_mic_power::Status *);
#endif
