// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the production wrapper with fake mapping, observer and lock APIs.
// Only IOKit includes and the ARM64 DSB are adapted by the test runner.
#include "PicassoMicPower.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
using namespace picasso_mic_power;
namespace fake {
uint32_t core[0x2000/4]={},owners[0x1000/4]={},channels[EndpointCount][0x80/4]={};
uint8_t registers[EndpointCount][256]={};
unsigned memoryCalls,mapCalls,readCalls,liveMaps,liveMemory;
unsigned failMemory,failMap,zeroVirtual,failRead;
int readError=3;
bool initialized=true;
std::string log;
const unsigned apids[EndpointCount]={1,2,3,4};
void *address(uint64_t base,uint64_t length) {
    if(base==0x0c440000){assert(length==0x2000);return core;}
    if(base==0x0c40a000){assert(length==0x1000);return owners;}
    for(unsigned e=0;e<EndpointCount;++e)if(base==0x0e600000+apids[e]*0x80) {
        assert(length==0x80);return channels[e];
    }
    assert(false);return nullptr;
}
void reset() {
    assert(!liveMaps && !liveMemory);
    memoryCalls=mapCalls=readCalls=0;
    failMemory=failMap=zeroVirtual=failRead=0;readError=3;initialized=true;
    memset(core,0,sizeof(core));memset(owners,0,sizeof(owners));
    memset(registers,0,sizeof(registers));log.clear();
    core[0]=0x50010000;
    const unsigned ppids[EndpointCount]={0x001,0x401,0x11d,0x5a0};
    for(unsigned e=0;e<EndpointCount;++e) {
        core[(0x900+apids[e]*4)/4]=(ppids[e]<<8)|1;
        owners[(0x700+apids[e]*4)/4]=4;
    }
    registers[PmicA][4]=registers[PmicC][4]=0x51;
    registers[PmicA][5]=0x1e;registers[PmicC][5]=0x1f;
    registers[S4A][1]=5;registers[S4A][4]=3;registers[S4A][5]=0x0a;
    registers[BobC][1]=0;registers[BobC][4]=41;registers[BobC][5]=5;
    for(unsigned e=S4A;e<=BobC;++e) {
        registers[e][8]=registers[e][0x46]=0x80;registers[e][0x45]=6;
        unsigned mv=e==S4A?1800:3496;
        registers[e][0x40]=registers[e][0x42]=uint8_t(mv);
        registers[e][0x41]=registers[e][0x43]=uint8_t(mv>>8);
        if(e==BobC) {
            registers[e][0x42]=uint8_t(3520);registers[e][0x43]=uint8_t(3520>>8);
        }
    }
}
}
using IOLock=std::mutex;
IOLock *IOLockAlloc(){return new IOLock;}
void IOLockFree(IOLock *lock){delete lock;}
void IOLockLock(IOLock *lock){lock->lock();}
void IOLockUnlock(IOLock *lock){lock->unlock();}
enum { kIOMapAnywhere=1,kIOMapInhibitCache=2 };
struct IOMemoryMap {
    void *value;bool zero;
    IOMemoryMap(void *v,bool z):value(v),zero(z){++fake::liveMaps;}
    uintptr_t getVirtualAddress(){return zero?0:reinterpret_cast<uintptr_t>(value);}
    void release(){assert(fake::liveMaps);--fake::liveMaps;delete this;}
};
struct IODeviceMemory {
    void *value;
    static IODeviceMemory *withRange(uint64_t base,uint64_t length) {
        ++fake::memoryCalls;
        if(fake::memoryCalls==fake::failMemory)return nullptr;
        ++fake::liveMemory;return new IODeviceMemory{fake::address(base,length)};
    }
    IOMemoryMap *map(unsigned options) {
        assert(options==(kIOMapAnywhere|kIOMapInhibitCache));++fake::mapCalls;
        if(fake::mapCalls==fake::failMap)return nullptr;
        return new IOMemoryMap(value,fake::mapCalls==fake::zeroVirtual);
    }
    void release(){assert(fake::liveMemory);--fake::liveMemory;delete this;}
};
void IOLog(const char *format,...) {
    char buffer[1024];va_list ap;va_start(ap,format);
    int n=vsnprintf(buffer,sizeof(buffer),format,ap);va_end(ap);
    assert(n>=0 && size_t(n)<sizeof(buffer));fake::log+=buffer;
}
extern "C" bool picasso_spmi_init(){return fake::initialized;}
extern "C" int picasso_spmi_read8(volatile uint32_t *channel,uint8_t reg,uint8_t &value) {
    ++fake::readCalls;
    if(fake::readCalls==fake::failRead)return fake::readError;
    for(unsigned e=0;e<EndpointCount;++e)if(channel==fake::channels[e]) {
        value=fake::registers[e][reg];return 0;
    }
    assert(false);return -71;
}
// Deliberately provide no write API: any accidental peripheral write fails link.
#include "picasso-mic-power-backend.inc"
static Status snapshot(){Status s={};picasso_mic_rails_status(&s);return s;}
static void clean_failure(int expected) {
    assert(picasso_mic_rails_acquire()==expected);
    Status s=snapshot();assert(!s.qualified && !s.observing && !s.ownedVotes && s.error==expected);
    assert(!fake::liveMaps && !fake::liveMemory);
    assert(picasso_mic_rails_release()==0);
}
int main(int argc,char **argv) {
    assert(argc>=2);fake::reset();std::string scenario=argv[1];
    if(scenario=="success") {
        assert(!snapshot().qualified && !snapshot().observing);
        assert(picasso_mic_rails_acquire()==0 && fake::readCalls==52);
        assert(fake::mapCalls==6 && fake::liveMaps==6 && !fake::liveMemory);
        Status s=snapshot();assert(s.qualified && s.observing && !s.ownedVotes);
        for(unsigned e=0;e<EndpointCount;++e)assert(s.owner[e]==4);
        assert(picasso_mic_rails_check()==0 && fake::readCalls==104);
        assert(picasso_mic_rails_acquire()==0 && fake::mapCalls==6);
        assert(picasso_mic_rails_release()==0 && !fake::liveMaps);
        assert(!snapshot().qualified && !snapshot().observing);
        unsigned reads=fake::readCalls;assert(picasso_mic_rails_check()==Unavailable && fake::readCalls==reads);
        assert(fake::log.find("vote_owned=0 peripheral_writes=0")!=std::string::npos);
    } else if(scenario=="mapping") {
        for(unsigned kind=0;kind<3;++kind)for(unsigned at=1;at<=6;++at) {
            fake::reset();
            if(kind==0)fake::failMemory=at;
            if(kind==1)fake::failMap=at;
            if(kind==2)fake::zeroVirtual=at;
            clean_failure(-12);assert(!fake::readCalls);
        }
        fake::reset();fake::initialized=false;clean_failure(Unavailable);assert(!fake::mapCalls);
        fake::reset();fake::core[0]=0x60000000;clean_failure(Unsupported);assert(!fake::readCalls);
        fake::reset();fake::core[(0x900+fake::apids[S4A]*4)/4]=0;
        clean_failure(Unavailable);assert(!fake::readCalls);
    } else if(scenario=="busfault") {
        for(unsigned at=1;at<=52;++at) {
            fake::reset();fake::failRead=at;clean_failure(-5);
            assert(fake::readCalls==at && snapshot().transportError==3);
        }
        fake::reset();assert(picasso_mic_rails_acquire()==0);
        fake::failRead=fake::readCalls+1;
        assert(picasso_mic_rails_check()==-5 && !snapshot().qualified);
        assert(picasso_mic_rails_release()==0 && !fake::liveMaps);
    } else if(scenario=="qualification") {
        fake::registers[BobC][0x42]=uint8_t(3295);fake::registers[BobC][0x43]=uint8_t(3295>>8);clean_failure(Voltage);
        fake::reset();assert(picasso_mic_rails_acquire()==0);
        fake::registers[S4A][8]=0;
        assert(picasso_mic_rails_check()==NotReady && !snapshot().qualified);
        assert(picasso_mic_rails_release()==0 && !fake::liveMaps);
        fake::reset();assert(picasso_mic_rails_acquire()==0);
        fake::core[(0x900+fake::apids[PmicA]*4)/4]=0;
        unsigned reads=fake::readCalls;
        assert(picasso_mic_rails_check()==Protocol && fake::readCalls==reads && !snapshot().qualified);
        assert(picasso_mic_rails_release()==0 && !fake::liveMaps);
    } else if(scenario=="timeout") {
        assert(argc==3);fake::failRead=unsigned(atoi(argv[2]));fake::readError=-1;
        assert(picasso_mic_rails_acquire()==-60);
        assert(!snapshot().qualified && !snapshot().observing && snapshot().transportError==-1);
        assert(fake::liveMaps==6 && !fake::liveMemory);
        unsigned reads=fake::readCalls,maps=fake::mapCalls;
        assert(picasso_mic_rails_acquire()==-60 && picasso_mic_rails_check()==-60);
        assert(picasso_mic_rails_release()==0 && fake::liveMaps==6);
        assert(picasso_mic_rails_acquire()==-60 && fake::readCalls==reads && fake::mapCalls==maps);
    } else assert(false);
    printf("PASS production MicPower wrapper %s; no peripheral write implementation linked\n",scenario.c_str());
}
