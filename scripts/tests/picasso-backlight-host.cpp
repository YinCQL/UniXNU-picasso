// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoBacklightCore.h"
using picasso_backlight::State;
struct Bus {
    unsigned operations=0, fail=0, mismatch=0, writes=0, sleeps=0;
    unsigned trace[128]={};
    uint8_t regs[2][256]={};
    Bus() {
        regs[0][0x46]=0x85;
        regs[1][0x46]=0x70; regs[1][0x50]=0x80; regs[1][0x51]=1;
        regs[1][0x53]=0x34; regs[1][0x54]=2;
    }
    int read(bool sink,uint8_t reg,uint8_t &v) {
        if(++operations==fail)return -1;
        v=regs[sink][reg]; if(operations==mismatch)v^=1;
        return 0;
    }
    int write(bool sink,uint8_t reg,uint8_t v) {
        if(++operations==fail)return -1;
        trace[writes++]=(unsigned(sink)<<16)|(unsigned(reg)<<8)|v;
        regs[sink][reg]=v; return 0;
    }
    void soft_start() {++sleeps;}
};
int main() {
    unsigned cases=0;
    Bus b;State s;uint8_t original[2][256];memcpy(original,b.regs,sizeof original);
    assert(!s.adopt(b) && s.on && s.ready && !b.writes);++cases;
    assert(!s.set(b,true) && !b.writes);++cases;
    assert(!s.set(b,false) && !s.on && b.writes==2);
    assert(b.trace[0]==0xef03 && b.trace[1]==0x4605 && !b.sleeps);++cases;
    assert(!s.set(b,false) && b.writes==2);++cases;
    assert(!s.set(b,true) && s.on && b.writes==5 && b.sleeps==1);
    assert(b.trace[2]==0xef00 && b.trace[3]==0x4685 && b.trace[4]==0x1b600);++cases;
    assert(!memcmp(original,b.regs,sizeof original));++cases;
    // Every adoption failure must be read-only, including invalid stock config.
    for(unsigned n=1;n<=12;++n) {
        Bus bus;State state;bus.fail=n;assert(state.adopt(bus) && !state.ready && !bus.writes);++cases;
    }
    const unsigned bad[][3]={{0,8,1},{0,0x46,0},{1,0x46,0x80},{1,0x50,0},
        {1,0x51,2},{1,0x51,3},{1,0x52,1},{1,0x73,1},{1,0x83,1},{1,0x93,1},{1,0x73,2},{1,0x83,2},{1,0x93,2},{1,0xb0,0x40},{1,0x54,0xf0}};
    for(auto &entry:bad) {
        Bus bus;State state;bus.regs[entry[0]][entry[1]]=uint8_t(entry[2]);
        assert(state.adopt(bus) && !state.ready && !bus.writes);++cases;
    }
    // Exercise every transport operation in both transitions. A partial write
    // poisons the state; a second request must issue no bus transactions.
    for(unsigned e=0;e<2;++e) {
        bool enable=e!=0;
        for(unsigned n=1;n<=(enable?11U:8U);++n) {
            Bus bus;State state;assert(!state.adopt(bus));
            if(enable)assert(!state.set(bus,false));
            bus.fail=bus.operations+n;
            assert(state.set(bus,enable) && state.failed);
            unsigned count=bus.operations;assert(state.set(bus,!enable) && bus.operations==count);++cases;
        }
        for(unsigned n=6;n<=(enable?10U:8U);n+=2) {
            Bus bus;State state;assert(!state.adopt(bus));
            if(enable)assert(!state.set(bus,false));
            bus.mismatch=bus.operations+n;
            assert(state.set(bus,enable)==picasso_backlight::mismatch && state.failed);++cases;
        }
    }
    for(unsigned src=0;src<2;++src) {
        Bus bus;State state;bus.regs[1][0x51]=uint8_t(src);
        assert(!state.adopt(bus) && !state.set(bus,false) && !state.set(bus,true));
        assert(bus.regs[1][0x51]==src);++cases;
        bus.regs[1][0x51]^=1;
        assert(state.set(bus,false) && state.failed);++cases;
    }
    const unsigned changed[]={8,0x46,0xb0};
    for(unsigned reg:changed) {
        Bus bus;State state;assert(!state.adopt(bus));
        bus.regs[reg==0xb0][reg]^=reg==0xb0?0x40:1;
        assert(state.set(bus,false) && state.failed && !bus.writes);++cases;
    }
    // The WLED5 sequence is two brightness bytes, then MOD_A sync 0 -> 1.
    const unsigned goals[]={42,1023,2048,4095};
    for(unsigned goal : goals) {
        Bus bus; State state; bus.regs[1][0x65]=0xa1;
        assert(!state.adopt(bus)); assert(!state.brightness(bus,goal));
        assert(state.level==goal && bus.writes==4);
        assert(bus.trace[0]==(0x15300|(goal&255)) && bus.trace[1]==(0x15400|(goal>>8)));
        assert(bus.trace[2]==0x165a0 && bus.trace[3]==0x165a1);
        unsigned writes=bus.writes;assert(!state.brightness(bus,goal) && bus.writes==writes);++cases;
    }
    Bus sample; State sampleState; assert(!sampleState.adopt(sample));
    unsigned begin=sample.operations;assert(!sampleState.brightness(sample,4095));
    unsigned operations=sample.operations-begin;
    for(unsigned n=1;n<=operations;++n) {
        Bus bus; State state;assert(!state.adopt(bus));bus.fail=bus.operations+n;
        assert(state.brightness(bus,4095) && state.failed);
        unsigned count=bus.operations;assert(state.brightness(bus,1023) && bus.operations==count);++cases;
    }
    const unsigned mismatches[]={6,9,11,13,15,16,17};
    for(unsigned n : mismatches) {
        Bus bus; State state;assert(!state.adopt(bus));bus.mismatch=bus.operations+n;
        assert(state.brightness(bus,4095) && state.failed);++cases;
    }
    const unsigned badGoals[]={0,1,41,4096,0xffffffff};
    for(unsigned badGoal : badGoals) {
        Bus bus; State state;assert(!state.adopt(bus));unsigned before=bus.operations;
        assert(state.brightness(bus,badGoal) && bus.operations==before && !bus.writes);++cases;
    }
    {
        Bus bus;State state;assert(!state.adopt(bus));assert(!state.set(bus,false));
        assert(!state.brightness(bus,42) && !state.on);assert(!state.set(bus,true));++cases;
        bus.regs[1][0x51]^=1;assert(state.brightness(bus,4095) && state.failed);++cases;
    }
    {
        Bus bus;State state;assert(!state.adopt(bus));bus.regs[1][0x65]=2;
        assert(state.brightness(bus,42) && state.failed && !bus.writes);++cases;
    }
    picasso_backlight::Fade fade;
    fade.begin(1023,4095,1000,100);assert(fade.value(100)==1023 && fade.value(600)==2559);
    assert(fade.value(1100)==4095 && !fade.active(1100));++cases;
    fade.begin(4095,0,1000,0);assert(fade.value(500)==2048 && fade.value(999)==42 && fade.value(1000)==0);++cases;
    fade.begin(42,4095,10000,0);assert(fade.duration==5000 && fade.value(5000)==4095);++cases;
    fade.begin(1023,1,0,0);assert(fade.value(0)==42 && !fade.active(0));++cases;
    uint32_t maps[512]={},owners[512]={};
    auto read=[&](bool owner,uint32_t offset) {return owner?owners[(offset-0x700)/4]:maps[(offset-0x900)/4];};
    assert(picasso_backlight::find_apid(read,0x5d8)==-1);
    maps[13]=0x5d800;owners[13]=1;assert(picasso_backlight::find_apid(read,0x5d8)==-1);
    owners[13]=0;assert(picasso_backlight::find_apid(read,0x5d8)==13);
    maps[14]=0x5d800;assert(picasso_backlight::find_apid(read,0x5d8)==-2);cases+=4;
    printf("PASS WLED: %u state, ordering, readback, failure and ownership cases\n",cases);
}
