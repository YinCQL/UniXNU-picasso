// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include "PicassoChargerCore.h"
using namespace picasso_charger;
struct Bus {
    uint8_t data[256]={};
    unsigned reads=0,writes=0,failRead=0,failWrite=0,ignoreWrite=0;
    std::vector<unsigned> written;
    Bus(){data[currentLimit]=100;data[options]=0xff;data[pulses]=0xff;
        data[allowance]=0xf7;data[allowOverride]=0xa8;
        data[0x40]=1;data[0x42]=1;data[0x65]=0x9f;data[0x66]=0xf7;}
    int chargerRead(uint8_t reg,uint8_t &value){
        if(++reads==failRead)return -70;
        value=data[reg];return 0;
    }
    int chargerWrite(uint8_t reg,uint8_t value){
        if(++writes==failWrite)return -71;
        assert(reg==command||reg==allowOverride||reg==pulses||reg==allowance||reg==options||reg==currentLimit);
        written.push_back(reg);
        if(writes!=ignoreWrite){
            if(reg==command){assert(value==8);data[9]=1;} // Command is self-clearing.
            else data[reg]=value;
        }
        return 0;
    }
};
static void preserve(const Bus &b){
    // Source overrides, USB current mode and suspend must not be touched.
    assert(b.data[0x40]==1&&b.data[0x42]==1&&b.data[0x65]==0x9f&&b.data[0x66]==0xf7);
    assert((b.data[options]&~0x6c)==(0xff&~0x6c));
    assert((b.data[options]&0x6c)==8);
    assert((b.data[allowance]&0xf0)==0xf0&&(b.data[allowOverride]&0xf0)==0xa0);
}
int main(){
    static_assert(maxInputMv==5000 && maxInputMa==3000);
    Bus baseline;Result first=restrictInput(baseline);
    assert(first.verified&&!first.error&&first.configuredMa==3000);
    assert(baseline.written.front()==currentLimit);preserve(baseline);
    unsigned writes=baseline.writes;Result repeat=restrictInput(baseline);
    assert(repeat.verified&&!repeat.error&&!repeat.writes&&baseline.writes==writes);
    // Exhaust all 50 mA codes, including zero. No limit below 3 A rises.
    for(unsigned value=0;value<=255;++value){
        Bus b;b.data[currentLimit]=uint8_t(value);auto q=restrictInput(b);
        assert(q.verified&&!q.error&&q.configuredMa==(value>60?3000:value*50));
        preserve(b);
    }
    // Exhaust the inherited detector configuration: BC1.2 must be enabled,
    // QC disabled and all unrelated bits preserved without raising the ICL.
    for(unsigned bits=0;bits<=255;++bits){
        Bus b;b.data[options]=uint8_t(bits);b.data[currentLimit]=10;
        auto q=restrictInput(b);assert(q.verified&&q.configuredMa==500);
        assert((b.data[options]&0x6c)==8&&(b.data[options]&~0x6c)==(bits&~0x6c));
        assert(b.data[0x40]==1&&b.data[0x42]==1&&b.data[0x65]==0x9f&&b.data[0x66]==0xf7);
        auto repeat=restrictInput(b);assert(repeat.verified&&!repeat.writes);
    }
    {Bus recorded;recorded.data[options]=0;assert(restrictInput(recorded).verified);
     unsigned enable=0;for(unsigned i=0;i<recorded.written.size();++i)if(recorded.written[i]==options)enable=i+1;
     assert(enable);Bus b;b.data[options]=0;b.ignoreWrite=enable;
     auto q=restrictInput(b);assert(q.error==-50&&!q.verified);}
    // The complete BC1.2 classification byte cannot enable a higher level.
    for(unsigned source=0;source<128;++source){
        Bus b;b.data[8]=uint8_t(source);b.data[9]=1;b.data[currentLimit]=10;
        auto q=restrictInput(b);assert(q.verified&&q.configuredMa==500);preserve(b);
        for(unsigned reg:b.written)assert(reg!=command);
    }
    for(unsigned source:{0x28u,0x48u})for(unsigned voltage:{2u,4u}){
        Bus b;b.data[8]=uint8_t(source);b.data[9]=uint8_t(voltage);
        auto q=restrictInput(b);assert(q.verified&&b.data[9]==1);
        auto commandAt=b.written.size();auto disableAt=b.written.size();
        for(unsigned i=0;i<b.written.size();++i){
            if(b.written[i]==command)commandAt=i;
            if(b.written[i]==options&&disableAt==b.written.size())disableAt=i;
        }
        assert(disableAt<commandAt);preserve(b);
        auto again=restrictInput(b);assert(again.verified&&!again.writes);
    }
    Bus successful;successful.data[8]=0x28;successful.data[9]=2;
    auto success=restrictInput(successful);assert(success.verified);
    for(unsigned fail=1;fail<=successful.reads;++fail){
        Bus b;b.data[8]=0x28;b.data[9]=2;b.failRead=fail;
        auto q=restrictInput(b);assert(!q.verified&&q.error==-70&&b.reads==fail);
    }
    for(unsigned fail=1;fail<=successful.writes;++fail){
        Bus b;b.data[8]=0x28;b.data[9]=2;b.failWrite=fail;
        auto q=restrictInput(b);assert(!q.verified&&q.error==-71&&b.writes==fail);
    }
    // Every configuration write is read back; command completion is not a
    // configuration readback and requires actual VBUS observation on hardware.
    for(unsigned ignore=1;ignore<=first.writes;++ignore){
        Bus b;b.ignoreWrite=ignore;auto q=restrictInput(b);assert(!q.verified&&q.error==-50);
    }
    puts("PASS 5V/3A restrictions, all current/source codes, lower-limit preservation, QC order, failure/readback and idempotence; register peer only");
}
