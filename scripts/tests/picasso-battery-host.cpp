// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>
#include "PicassoBatteryCore.h"
struct Bus {
    unsigned reads=0,failAt=0;bool changing=false;
    uint8_t qg[256]={},usb[256]={},chg[256]={};
    Bus(){qg[8]=0x81;qg[0xbf]=170;qg[0xc0]=0x46;qg[0xc1]=0x50;qg[0xc2]=0x18;qg[0xc3]=0xfc;usb[0x10]=0x10;chg[6]=3;}
    int read(unsigned ch,uint8_t reg,uint8_t &v){
        if(++reads==failAt)return -40;
        assert(ch<3);assert(ch!=0||reg==8||reg==0xbf||(reg>=0xc0&&reg<=0xc3));
        v=(ch==0?qg:ch==1?usb:chg)[reg];if(changing&&reg==0xc0 && reads%8==1)v^=1;return 0;
    }
};
int main(){
    assert(picasso_battery::voltage_uv(0)==0&&picasso_battery::voltage_uv(20550)==3999790);
    assert(picasso_battery::current_ua(1000,3)==152588&&picasso_battery::current_ua(64536,3)==-152588);
    assert(picasso_battery::current_ua(1000,4)==305176&&picasso_battery::current_ua(32768,4)==-10000007);
    for(unsigned n=1;n<=12;++n){Bus b;b.failAt=n;picasso_battery::Sample s;s.soc=19;assert(picasso_battery::sample(b,3,s)==-40&&s.soc==19);}
    {Bus b;picasso_battery::Sample s;assert(!picasso_battery::sample(b,3,s)&&s.voltageUv==3999790&&s.currentUa==-152588&&s.soc==170&&s.usb==16&&s.charger==3);}
    {Bus b;picasso_battery::Sample s;assert(picasso_battery::sample(b,2,s)==-20&&!b.reads);}
    {Bus b;b.changing=true;picasso_battery::Sample s;assert(picasso_battery::sample(b,3,s)==-21&&b.reads==28);}
    for(unsigned status:{0u,1u,128u}){Bus b;b.qg[8]=uint8_t(status);picasso_battery::Sample s;assert(picasso_battery::sample(b,3,s)==-22);}
    puts("PASS QG signed current/voltage units, supported subtype, stable tuple, 12 transport failures and validity rejection; SOC remains raw");
}
