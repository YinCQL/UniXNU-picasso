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
    uint8_t statusDelta=0;unsigned statusReads=0;
    Fake() {
        reg[PmicA][4]=reg[PmicC][4]=0x51;
        reg[PmicA][5]=0x1e;reg[PmicC][5]=0x1f;
        reg[S4A][1]=5;reg[S4A][4]=3;reg[S4A][5]=0x0a;
        reg[BobC][1]=0;reg[BobC][4]=41;reg[BobC][5]=5;
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
        if(e>=S4A && r==8 && (++statusReads&1)==0)v^=statusDelta;
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
        int expected=(tear==12 || tear==23 || tear==34 || tear==45)?0:Unstable;
        assert(qualify(b,s)==expected && bool(s.qualified)==(expected==0));
        if(expected==Unstable && tear>=9)assert(s.mismatch[tear<=30?0:1]);
    }
    // Firmware checks READY (bit 7) and unified OCP (bit 5), not equality
    // of the full dynamic status byte. Retain all configuration comparisons.
    for(unsigned delta:{1U,2U,4U,8U,16U,64U}) {
        Fake b;b.reg[S4A][8]=0x86;b.reg[BobC][8]=0x86;b.statusDelta=uint8_t(delta);
        assert(!qualify(b,s) && s.qualified);
    }
    for(unsigned delta:{32U,128U}) {
        Fake b;b.statusDelta=uint8_t(delta);
        assert(qualify(b,s)!=0 && !s.qualified);
    }
    for(unsigned endpoint=S4A;endpoint<=BobC;++endpoint) {
        Fake b;b.reg[endpoint][8]|=0x20;
        assert(qualify(b,s)==NotReady && !s.qualified);
        b=Fake{};b.reg[endpoint][1]=endpoint==S4A?3:1;
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
        for(unsigned mv:{0U,target-1,endpoint==S4A?1801U:3961U,65535U}) {
            b=Fake{};b.voltage(endpoint,mv);
            assert(qualify(b,s)==Voltage && !s.qualified);
        }
        // An in-range request cannot qualify an out-of-range accepted value.
        b=Fake{};unsigned below=target-1;
        b.reg[endpoint][0x42]=uint8_t(below);b.reg[endpoint][0x43]=uint8_t(below>>8);
        assert(qualify(b,s)==Voltage && !s.qualified);
    }
    // Captured PM8150/PM8150L register values from the first board boot.
    // The shared BOB request need not equal its accepted setting or the
    // microphone consumer's minimum. Neither readback is an ADC sample.
    Fake board;board.reg[S4A][1]=4;board.reg[S4A][8]=0x96;
    board.reg[BobC][8]=0x86;board.voltage(BobC,3496);
    board.reg[BobC][0x42]=uint8_t(3520);board.reg[BobC][0x43]=uint8_t(3520>>8);
    assert(qualify(board,s)==0 && s.qualified && s.rail[1].validMv==3520);
    for(unsigned requested:{3296U,3496U,3960U})for(unsigned accepted:{3296U,3520U,3960U}) {
        Fake b;b.voltage(BobC,requested);
        b.reg[BobC][0x42]=uint8_t(accepted);b.reg[BobC][0x43]=uint8_t(accepted>>8);
        assert(qualify(b,s)==0 && s.qualified);
    }
    // Independently reject either side of an unsafe or stale transition.
    for(unsigned mv:{3008U,3295U,3961U,65535U})for(bool accepted:{false,true}) {
        Fake b;b.voltage(BobC,3520);unsigned reg=accepted?0x42:0x40;
        b.reg[BobC][reg]=uint8_t(mv);b.reg[BobC][reg+1]=uint8_t(mv>>8);
        assert(qualify(b,s)==Voltage && !s.qualified);
    }
    for(unsigned subtype:{3U,4U,6U,255U}) {
        Fake b;b.reg[BobC][5]=uint8_t(subtype);
        assert(qualify(b,s)==Unsupported && !s.qualified);
    }
    for(unsigned revision:{1U,2U,3U,255U}) {
        Fake b;b.reg[BobC][1]=uint8_t(revision);
        assert(qualify(b,s)==Unsupported && !s.qualified);
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
