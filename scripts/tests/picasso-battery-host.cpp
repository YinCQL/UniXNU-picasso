// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>
#include "PicassoBatteryCore.h"
using namespace picasso_battery;
struct Bus {
    unsigned reads=0,writes=0,waits=0,failAt=0,failWrite=0;bool changing=false,timeout=false,wrongChannel=false;
    uint8_t data[5][256]={};
    Bus(){data[0][8]=0x81;data[0][0xbf]=239;data[0][0xc0]=0x46;data[0][0xc1]=0x50;
        data[0][0xc2]=0x18;data[0][0xc3]=0xfc;data[1][0x10]=0x10;data[2][6]=3;data[3][0x0b]=0x11;
        data[4][0x50]=0x72;data[4][0x51]=0x38;}
    int read(unsigned ch,uint8_t reg,uint8_t &v){
        if(++reads==failAt)return -40;assert(ch<5);
        if(ch==0)assert(reg==8||reg==0xbf||(reg>=0xc0&&reg<=0xc3));
        v=data[ch][reg];if(changing&&ch==0&&reg==0xc0)v^=uint8_t((reads%21)>10);
        if(wrongChannel&&ch==4&&reg==0x44)v=0;return 0;
    }
    int write(uint8_t reg,uint8_t v){
        if(++writes==failWrite)return -41;assert(reg>=0x42 && reg<=0x47);data[4][reg]=v;
        if(reg==0x47)data[4][8]=2;return 0;
    }
    void wait(){++waits;if(!timeout)data[4][8]=1;}
};
int main(){
    assert(voltage_uv(20550)==3999790);
    assert(current_ua(1000,3)==152588&&current_ua(64536,3)==-152588);
    assert(current_ua(1000,4)==305176&&current_ua(32768,4)==-10000007);
    for(unsigned n=1;n<=21;++n){Bus b;b.failAt=n;Sample s;s.soc=19;assert(sample(b,4,s)==-40&&s.soc==19);}
    {Bus b;Sample s;assert(!sample(b,4,s)&&s.voltageUv==3999790&&s.currentUa==-305176&&s.soc==239);}
    {Bus b;Sample s;assert(sample(b,2,s)==-20&&!b.reads);}
    {Bus b;b.changing=true;Sample s;assert(sample(b,4,s)==-21&&b.reads==63);}
    for(unsigned status:{0u,1u}){Bus b;b.data[0][8]=uint8_t(status);Sample s;assert(sample(b,4,s)==-22);}
    {Bus b;b.data[0][8]=0x80;Sample s;assert(!sample(b,4,s));}
    assert(temperature_deci_c(0)==-10000 && temperature_deci_c(0x70e4)==-10000);
    assert(temperature_deci_c(0x3872)>=248 && temperature_deci_c(0x3872)<=252);
    assert(battery_id_ohm(0x3872)==100000&&battery_id_ohm(0x70e4)==0);
    for(unsigned c=0;c<6;++c)for(unsigned r=0;r<56;++r)
        assert(ocv_soc(profileOcv[r][c]*100,profileTemperature[c]*10)==profileSoc[r]);
    for(int t=-200;t<=600;t+=10){int previous=0;
        for(int uv=2500000;uv<=4600000;uv+=1000){int soc=ocv_soc(uv,t);assert(soc>=previous&&soc<=10000);previous=soc;}}
    for(unsigned mode=0;mode<8;++mode)for(unsigned plug=0;plug<2;++plug)for(unsigned flow=0;flow<2;++flow){
        Estimator e;Snapshot o;Sample s;s.status=0x81;s.voltageUv=4000000;s.currentUa=flow?-500000:500000;
        s.usb=plug?16:0;s.path=17;s.charger=uint8_t(mode);e.update(s,1000,250,true,4000000,o);
        assert(o.charging==(plug&&mode>=1&&mode<=4));assert(o.milliamps==(flow?500:-500));assert(!o.full);
    }
    Sample s;s.status=0x81;s.voltageUv=3846400;s.currentUa=450000;s.charger=3;
    Estimator e;Snapshot o;e.update(s,1000,250,true,3846400,o);assert(o.percent==50);
    for(uint64_t ms=6000;ms<=3601000;ms+=5000)e.update(s,ms,250,true,0,o);
    assert(o.percent==40&&o.remainingMah==1800&&o.minutes==240);
    int64_t before=e.remainingUaMs;e.gap();e.update(s,8000000,250,true,0,o);assert(e.remainingUaMs==before);
    s.currentUa=-450000;s.usb=16;s.path=17;
    for(uint64_t ms=8005000;ms<=11600000;ms+=5000)e.update(s,ms,250,true,0,o);
    assert(o.percent==50&&o.charging&&o.minutes==-1);
    s.charger=5;s.voltageUv=4370000;s.currentUa=100000;e.update(s,11605000,250,true,0,o);assert(!o.full);
    s.currentUa=0;e.update(s,11610000,250,true,0,o);assert(o.full&&o.percent==100);
    s.thermal=8;e.update(s,11615000,250,true,0,o);assert(!o.full&&!o.charging);
    s.status=0x80;e.update(s,11620000,0,false,0,o);assert(!e.ready&&!o.present);
    for(unsigned n=1;n<=10;++n){Bus b;b.failAt=n;uint16_t out=9;assert(adc_read(b,0x4a,out)==-40&&out==9);}
    for(unsigned n=1;n<=6;++n){Bus b;b.failWrite=n;uint16_t out=9;assert(adc_read(b,0x4a,out)==-41&&out==9);}
    {Bus b;uint16_t v=0;assert(!adc_read(b,0x4a,v)&&v==0x3872&&b.waits==1&&b.writes==6);assert(b.data[4][0x42]==0x18&&b.data[4][0x45]==2);}
    {Bus b;b.data[4][8]=2;uint16_t v=9;assert(adc_read(b,0x4a,v)==-31&&!b.writes&&v==9);}
    {Bus b;b.timeout=true;uint16_t v=9;assert(adc_read(b,0x4a,v)==-34&&b.waits==10&&v==9);}
    {Bus b;b.wrongChannel=true;uint16_t v=9;assert(adc_read(b,0x4a,v)==-32&&v==9);}

    // Reproduce the observed USB=10/path=95/CHGR=03 with net discharge.
    {Estimator e;Snapshot o;Sample s;s.status=0x81;s.usb=0x10;s.path=0x95;s.charger=3;
     s.voltageUv=4127277;s.currentUa=90637;e.update(s,1000,300,true,4127277,o);
     assert(o.external&&o.capable&&o.charging&&!o.netCharging&&o.milliamps==-90&&o.minutes==-1);
     const auto remaining=e.remainingUaMs;e.update(s,6000,300,true,0,o);
     assert(e.remainingUaMs<remaining&&o.charging);
     for(unsigned bit:{1u,2u,4u,8u}){s.usb=uint8_t(0x10|bit);e.update(s,11000,300,true,0,o);assert(!o.capable&&!o.charging);}
     s.usb=0x10;s.path=0xd5;e.update(s,16000,300,true,0,o);assert(!o.charging);
     s.usb=0;s.path=0;e.update(s,21000,300,true,0,o);assert(!o.external&&!o.charging&&o.minutes>0);}
    {Bus b;Input i;b.data[1][7]=1;b.data[1][8]=1;b.data[3][8]=10;b.data[3][0x0a]=0x41;
     assert(!input_status(b,i)&&i.statusValid&&i.icl==10&&adapter_family(i,true)==0xe0004000u);
     for(unsigned n=1;n<=8;++n){Bus f;Input unchanged;unchanged.icl=99;f.failAt=n;
         assert(input_status(f,unchanged)==-40&&unchanged.icl==99&&!unchanged.statusValid);}
     b.data[3][8]=101;assert(input_status(b,i)==-36);}
    {Input i;i.statusValid=true;i.apsd=1;
     for(unsigned result:{2u,8u,0x28u,0x48u}){i.result=uint8_t(result);assert(adapter_family(i,true)==0xe0004004u);}
     i.result=4;assert(adapter_family(i,true)==0xe0004005u);i.result=3;assert(adapter_family(i,true)==0xe0004007u);
     i.apsd=0;assert(adapter_family(i,true)==0xe0004007u&&adapter_family(i,false)==0);}
    assert(adc_voltage_uv(0x3872,16)==15000000&&adc_voltage_uv(0x3872,5)==4687500);
    for(unsigned channel:{7u,8u}){Bus b;uint16_t value=77;
     assert(!adc_read(b,uint8_t(channel),value)&&b.data[4][0x42]==0x28&&b.data[4][0x45]==0);
     b.data[4][0x50]=0;b.data[4][0x51]=0;assert(!adc_read(b,uint8_t(channel),value)&&value==0);}
    {Bus b;uint16_t v=17;assert(adc_read(b,9,v)==-30&&!b.reads&&!b.writes);}
    // Warning hysteresis, fresh-observation persistence and recovery; no direct halt.
    {LowBattery policy;Snapshot s;s.present=true;s.percent=30;s.raw.voltageUv=3490000;s.timeMs=1000;
     policy.update(s);assert(!s.warn&&!s.critical&&s.refreshMs==1000);
     s.timeMs=3000;policy.update(s);assert(!s.warn);s.timeMs=4000;policy.update(s);assert(s.warn&&!s.critical);
     s.raw.voltageUv=3530000;s.timeMs=5000;policy.update(s);assert(s.warn);
     s.raw.voltageUv=3560000;s.timeMs=6000;policy.update(s);assert(!s.warn);
     s.percent=1;s.raw.voltageUv=3370000;s.timeMs=7000;policy.update(s);assert(!s.critical);
     s.timeMs=9000;policy.update(s);assert(!s.critical);s.timeMs=10000;policy.update(s);assert(s.critical&&s.warn);
     s.capable=true;s.timeMs=11000;policy.update(s);assert(!s.warn&&!s.critical);
     s.capable=false;s.raw.voltageUv=3000000;s.percent=60;s.timeMs=12000;policy.update(s);assert(!s.critical);
     s.timeMs=30000;policy.update(s);assert(!s.critical);s.timeMs=33000;policy.update(s);assert(s.critical);
     policy.reset();s.critical=false;s.timeMs=40000;s.temperature=-50;s.temperatureValid=true;
     policy.update(s);assert(!s.critical);s.timeMs=43000;policy.update(s);assert(!s.critical);}

    {struct ChangingInput:Bus {int read(unsigned ch,uint8_t reg,uint8_t &v){int result=Bus::read(ch,reg,v);if(reads==6)++v;return result;}} b;
     Input out;out.icl=99;assert(input_status(b,out)==-35&&!out.statusValid&&out.icl==99);}
    for(unsigned channel:{7u,8u}){
        for(unsigned n=1;n<=10;++n){Bus b;b.failAt=n;uint16_t value=99;assert(adc_read(b,uint8_t(channel),value)==-40&&value==99);}
        for(unsigned n=1;n<=6;++n){Bus b;b.failWrite=n;uint16_t value=99;assert(adc_read(b,uint8_t(channel),value)==-41&&value==99);}
        Bus b;b.timeout=true;uint16_t value=99;assert(adc_read(b,uint8_t(channel),value)==-34&&value==99);
    }
    puts("PASS complete QG snapshots, current sign, profile interpolation, one-hour charge/discharge, gaps, charger/thermal states, ADC failures and timeout");
}
