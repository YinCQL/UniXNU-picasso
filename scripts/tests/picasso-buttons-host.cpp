// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "PicassoButtonsCore.h"
using namespace picasso_buttons;
struct Bus {
    unsigned writes=0, reads=0, delays=0, dataReads=0, doneAt=0;
    uint32_t status=1, cmd=0;
    void write(unsigned off,uint32_t v) {assert(off==0);++writes;cmd=v;}
    uint32_t read(unsigned off) {
        if(off==8)return reads++>=doneAt?status:0;
        assert(off==0x18);++dataReads;return 0x1234;
    }
    void delay() {++delays;}
};
int main() {
    assert(!version_supported(0x40000000) && version_supported(0x50000000));
    assert(version_supported(0x50020000) && !version_supported(0x60000000));
    uint32_t maps[512]={},owners[512]={};
    maps[0]=pon_ppid<<8;owners[0]=1;
    maps[21]=pon_ppid<<8;maps[22]=pon_ppid<<8;owners[22]=2;
    maps[511]=(up_ppid<<8)|(1<<24);
    auto read=[&](bool owner,unsigned off) {
        assert(off>= (owner?owner_offset:map_offset));
        unsigned i=(off-(owner?owner_offset:map_offset))/4;assert(i<512);
        return owner?owners[i]:maps[i];
    };
    assert(find_apid(read,pon_ppid)==21 && find_apid(read,up_ppid)==511);
    assert(find_apid(read,0x013)==-1 && observer_offset(511)+0x1c<=0x10000);
    maps[23]=pon_ppid<<8;assert(find_apid(read,pon_ppid)==23);
    for(unsigned status=1;status<16;status+=2) {
        Bus b;b.status=status;b.doneAt=3;uint8_t v=0xab;
        int error=read8(b,0x10,v);
        assert(b.writes==1 && b.cmd==0x08000100 && b.delays==3);
        assert(error==int(status&0xe));
        assert(error?(v==0xab && b.dataReads==0):(v==0x34 && b.dataReads==1));
    }
    Bus timeout;timeout.status=0;uint8_t v=0xcd;
    assert(read8(timeout,0x46,v)==-1 && v==0xcd && timeout.delays==1000 && !timeout.dataReads);
    for(unsigned s=0;s<256;++s) {
        bool known=s==1||s==5||s==9||s==13||s==16||s==17;
        assert(gpio_input(16,s,0,0x80)==known);
        assert(!gpio_input(15,s,0,0x80) && !gpio_input(16,s,0,0));
        assert(!gpio_input(16,s,s>=16?1:0x10,0x80));
    }
    for(unsigned m=0;m<8;++m) {
        unsigned expected=((m&1)?2:0)|((m&2)?8:0)|((m&4)?4:0);
        assert(mask(m&3,(m&4)?0:1)==expected);
    }
    Debounce d;
    assert(!d.sample(2,100) && !d.sample(0,110) && !d.sample(2,120));
    assert(!d.sample(2,139) && d.sample(2,140) && d.stable==2);
    assert(!d.sample(2,100000)); // held, no repeat
    assert(!d.sample(6,100010));
    assert(!d.sample(4,100020));
    assert(d.sample(4,100030) && d.stable==6);
    assert(d.sample(0,100040) && d.stable==4); // power releases while volume bounces
    assert(!d.sample(4,100050));
    assert(!d.sample(0,100060));
    assert(d.sample(0,100080) && d.stable==0);
    d={};assert(!d.sample(14,0));assert(d.sample(14,20) && d.stable==14);
    Reports reports;unsigned attempts=0;bool success=false;
    auto send=[&](uint32_t bits){++attempts;assert(bits==2 || bits==0);return success;};
    assert(!reports.sample(2,100,send) && attempts==0);
    assert(!reports.sample(2,120,send) && attempts==1 && !reports.delivered);
    success=true;
    assert(reports.sample(2,130,send) && attempts==2 && reports.delivered==2);
    assert(!reports.sample(2,140,send) && attempts==2);
    assert(!reports.sample(0,150,send));success=false;
    assert(!reports.sample(0,170,send) && reports.delivered==2);
    success=true;assert(reports.sample(0,180,send) && reports.delivered==0);
    puts("PASS: v5 APID/owner bounds, observer errors/timeouts, GPIO identity, polarity, held/combination and independent debounce");
}
