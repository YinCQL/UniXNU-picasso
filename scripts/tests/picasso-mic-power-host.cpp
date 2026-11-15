// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PicassoMicPower.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace picasso_mic_power;
struct Fake {
    uint8_t reg[EndpointCount][256]={};
    unsigned reads=0,failAt=0,tearAt=0;
    Fake() {
        reg[PmicA][4]=reg[PmicC][4]=0x51;
        reg[PmicA][5]=0x1e;reg[PmicC][5]=0x1f;
        reg[S4A][1]=5;reg[S4A][4]=3;reg[S4A][5]=0x0a;
        reg[BobC][1]=3;reg[BobC][4]=41;reg[BobC][5]=3;
        for(unsigned e=S4A;e<=BobC;++e) {
            reg[e][8]=reg[e][0x46]=0x80;reg[e][0x45]=6;
            voltage(e,e==S4A?1800:3296);
        }
    }
    void voltage(unsigned e,unsigned mv) {
        reg[e][0x40]=reg[e][0x42]=uint8_t(mv);
        reg[e][0x41]=reg[e][0x43]=uint8_t(mv>>8);
    }
    int read(unsigned e,uint8_t r,uint8_t &v) {
        assert(e<EndpointCount);++reads;
        if(reads==failAt)return -60;
        v=reg[e][r];if(reads==tearAt)v^=1;
        return 0;
    }
};
struct Table {
    uint32_t map[512]={},owner[512]={};
    unsigned reads=0;
    uint32_t read(bool o,unsigned at) {
        ++reads;unsigned start=o?0x700:0x900;
        assert(at>=start && (at-start)/4<512 && !(at&3));
        return (o?owner:map)[(at-start)/4];
    }
    void set(unsigned apid,unsigned ppid,unsigned ee) {map[apid]=(ppid<<8)|1;owner[apid]=ee;}
    int find(unsigned ppid,uint8_t &ee) {return find_observer([&](bool o,unsigned a){return read(o,a);},uint16_t(ppid),ee);}
};
int main() {
    Status s={};Fake good;
    assert(qualify(good,s)==0 && s.qualified==1 && s.stage==Qualified);
    assert(s.rail[0].validMv==1800 && s.rail[1].validMv==3296);
    assert(good.reads==52 && s.ownedVotes==0);
    // Every failed read, including the last byte of the last comparison,
    // invalidates an earlier successful observation and preserves its error.
    for(unsigned fail=1;fail<=52;++fail) {
        Fake b;b.failAt=fail;s.qualified=1;
        assert(qualify(b,s)==-60 && !s.qualified && s.error==-60);
        assert(b.reads==fail);
    }
    for(unsigned endpoint=PmicA;endpoint<=PmicC;++endpoint)for(unsigned reg:{4U,5U}) {
        Fake b;b.reg[endpoint][reg]^=1;
        assert(qualify(b,s)==Unsupported && !s.qualified);
    }
    // Identity and rail observations may change between any two readbacks.
    for(unsigned tear=1;tear<=52;++tear) {
        Fake b;b.tearAt=tear;
        assert(qualify(b,s)==Unstable && !s.qualified);
    }
    for(unsigned endpoint=S4A;endpoint<=BobC;++endpoint) {
        Fake b;b.reg[endpoint][1]=endpoint==S4A?3:1;
        assert(qualify(b,s)==Unsupported && !s.qualified);
        b=Fake{};b.reg[endpoint][4]^=1;
        assert(qualify(b,s)==Unsupported && !s.qualified);
        b=Fake{};b.reg[endpoint][5]^=1;
        assert(qualify(b,s)==Unsupported && !s.qualified);
        for(unsigned reg:{8U,0x46U}) {
            b=Fake{};b.reg[endpoint][reg]=0;
            assert(qualify(b,s)==NotReady && !s.qualified);
        }
        b=Fake{};b.reg[endpoint][0x47]=1;
        assert(qualify(b,s)==NotReady && !s.qualified);
        for(unsigned mode=0;mode<8;++mode) {
            b=Fake{};b.reg[endpoint][0x45]=uint8_t(mode);
            assert(qualify(b,s)==((mode==4||mode==6||mode==7)?0:NotReady));
        }
        unsigned target=endpoint==S4A?1800:3296;
        for(unsigned mv:{0U,target-1,target+1,65535U}) {
            b=Fake{};b.voltage(endpoint,mv);
            assert(qualify(b,s)==Voltage && !s.qualified);
        }
        // An exact requested setting cannot qualify an old accepted setting.
        b=Fake{};b.reg[endpoint][0x42]^=1;
        assert(qualify(b,s)==Voltage && !s.qualified);
    }
    Table table;uint8_t ee=0;
    assert(table.find(0x11d,ee)==Unavailable);
    table.set(11,0x11d,4);
    assert(table.find(0x11d,ee)==11 && ee==4); // read-only observer of another EE
    table.set(18,0x11d,5);
    assert(table.find(0x11d,ee)==Protocol);
    table.set(23,0x11d,0);
    assert(table.find(0x11d,ee)==23 && ee==0); // stock APPS preference
    table.set(30,0x11d,0);
    assert(table.find(0x11d,ee)==Protocol);
    Fake again;assert(qualify(again,s)==0 && s.qualified);
    puts("PASS rail identity, requested/accepted voltage, readiness/modes, every read fault, torn observations, stale qualification and observer mapping; no peripheral writes");
}
