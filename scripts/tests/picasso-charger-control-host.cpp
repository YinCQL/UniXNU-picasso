// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <vector>
#include "PicassoBatteryCore.h"
#include "PicassoChargerCore.h"
using namespace picasso_charger;
struct Bus {
    uint8_t data[7][256]={};
    unsigned calls=0,fail=0,ignored=0,writes=0,reads=0;
    std::vector<unsigned> written;
    bool race=false;
    Bus(){data[1][0x70]=10;data[1][0x80]=4;data[5][6]=8;data[5][0xb]=9;
        data[1][0x10]=0x10;data[1][8]=8;data[3][0xb]=0x95;}
    int read(unsigned ch,uint8_t reg,uint8_t &v){++reads;if(++calls==fail)return -90;
        v=data[ch][reg];if(race&&ch==1&&reg==8)v=1;return 0;}
    int chargerRead(uint8_t reg,uint8_t &v){return read(1,reg,v);}
    int chargerWrite(uint8_t reg,uint8_t v){++writes;if(++calls==fail)return -91;
        assert(reg==0x70||reg==0x66||reg==0x42||reg==0x65);written.push_back(reg);
        if(writes!=ignored)data[1][reg]=v;return 0;}
};
struct Fixture {
    Bus bus;Controller c;picasso_battery::Sample s;picasso_battery::Input in;Source src;Result r;
    Fixture(){s.status=0x81;s.usb=0x10;s.path=0x95;s.charger=3;
        in.statusValid=true;in.apsd=1;in.result=8;in.voltageValid=true;in.millivolts=5000;
        assert(!sourceStatus(bus,src));r.verified=true;bus.calls=bus.reads=0;}
    Control step(uint64_t now=1000,int temp=250,bool valid=true){bus.data[5][6]=src.sink;bus.data[5][0xb]=src.misc;bus.data[5][0xd]=src.legacy;
        bus.data[6][6]=src.thermal;bus.data[6][7]=src.die;bus.data[6][8]=src.skin;bus.data[6][9]=src.connector;
        bus.data[1][0x40]=src.suspend;bus.data[1][0x80]=src.aiclOptions;
        return c.update(bus,s,in,src,temp,valid,now,r);}
    void active(){assert(step().state==1);auto out=step(2000);assert(out.verified&&out.targetMa==1600&&bus.data[1][0x70]==32);}
    void safe(){assert(bus.data[1][0x70]<=10&&(bus.data[1][0x66]&7)==2&&(bus.data[1][0x42]&1)&&!(bus.data[1][0x65]&16));}
};
int main(){
    {Fixture f;f.active();assert((f.bus.data[1][0x66]&7)==1&&!(f.bus.data[1][0x42]&1)&&(f.bus.data[1][0x65]&16));
     f.bus.written.clear();auto out=f.step(3000);assert(out.verified&&f.bus.written.empty());}
    for(unsigned result=0;result<128;++result){Fixture f;f.in.result=uint8_t(result);f.bus.data[1][8]=uint8_t(result);
        f.step();auto out=f.step(2000);unsigned want=result==1?500:result==4?1500:(result==2||result==8||result==0x28||result==0x48)?1600:0;
        assert(out.sourceMa==want);assert(f.bus.data[1][0x70]<= (want>500?want/50:10));}
    for(unsigned rp:{2u,4u}){Fixture f;f.src.sink=uint8_t(rp);f.step();auto out=f.step(2000);
        assert(out.verified&&out.targetMa==(rp==2?3000:1500));}
    {Fixture f;f.src.sink=2;f.src.legacy=2;f.active();}
    {Fixture f;f.src.sink=2;f.in.result=1;f.bus.data[1][8]=1;auto out=f.step();assert(out.targetMa==500&&f.bus.data[1][0x70]==10);}
    // Missing/unsafe measurement, source and protection states all revoke HC.
    for(unsigned kind=0;kind<16;++kind){Fixture f;f.active();
        int temp=250;bool valid=true;
        switch(kind){case 0:valid=false;break;case 1:temp=99;break;case 2:temp=450;break;
         case 3:f.src.valid=false;break;case 4:f.src.thermal=0x40;break;case 5:f.src.connector=2;break;
         case 6:f.src.suspend=1;break;case 7:f.src.aiclOptions=0;break;case 8:f.in.voltageValid=false;break;
         case 9:f.in.millivolts=5300;break;case 10:f.s.thermal=0x20;break;case 11:f.s.chargeError=2;break;
         case 12:f.s.usb=0;break;case 13:f.src.misc|=0x80;break;case 14:f.src.legacy=1;break;
         case 15:f.in.statusValid=false;break;}
        auto out=f.step(3000,temp,valid);assert(out.state==0||out.state==3);assert(out.verified);f.safe();}
    {Fixture f;f.active();assert(f.step(3000,400).targetMa==500);f.safe();
     assert(f.step(4000,390).targetMa==500);assert(f.step(5000,380).state==1);assert(f.step(6000,380).targetMa==1600);}
    {Fixture f;f.bus.data[1][0x70]=2;auto out=f.step();assert(out.targetMa==100&&f.bus.written.empty());}
    {Fixture f;f.step();assert(f.step(30000).state==1);f.safe();}
    {Fixture f;f.active();f.in.result=1;f.bus.data[1][8]=1;assert(f.step(3000).targetMa==500);f.safe();}
    {Fixture f;f.step();f.bus.race=true;auto out=f.step(2000);assert(!out.verified&&out.error==-56&&!out.fallbackError);f.safe();}
    Fixture base;base.step();base.bus.calls=base.bus.writes=0;base.step(2000);unsigned calls=base.bus.calls,writes=base.bus.writes;
    for(unsigned i=1;i<=calls;++i){Fixture f;f.step();f.bus.calls=0;f.bus.fail=i;
        auto out=f.step(2000);assert(!out.verified&&out.error);assert(!out.fallbackError);f.safe();}
    for(unsigned i=1;i<=writes;++i){Fixture f;f.step();f.bus.writes=0;f.bus.ignored=i;
        auto out=f.step(2000);assert(!out.verified&&out.error==-50);f.safe();}
    {Fixture f;f.active();for(unsigned ch=0;ch<7;++ch)if(ch!=1)for(unsigned reg=0;reg<256;++reg)
        assert(f.bus.data[ch][reg]==Bus{}.data[ch][reg]);}
    for(unsigned i=1;i<=24;++i){Bus bus;Source src;bus.fail=i;assert(sourceStatus(bus,src)&&!src.valid);}
    puts("PASS source-qualified 5V control, SDP/CDP/DCP/Rp, thermal hysteresis, lower limits, unplug/race, every control I/O fault and ignored write; register peer only");
}
