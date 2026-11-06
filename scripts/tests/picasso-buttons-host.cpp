// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "PicassoButtonsCore.h"
using namespace picasso_buttons;
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
    puts("PASS: v5 APID/owner bounds, GPIO identity, polarity, held/combination and independent debounce");
}
