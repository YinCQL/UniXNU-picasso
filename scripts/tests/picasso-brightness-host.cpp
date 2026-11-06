// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "picasso-brightness-api.h"
#include "PicassoBacklightCore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static uint64_t clockMs;
static unsigned allocations,failAllocation,adds,failAdd,locks,propertyCalls,failProperty;
static bool applePresent=true,timerFailure=false;
static unsigned timerCount;
static IOTimerEventSource *timers[2];
static bool failAlloc() { return ++allocations==failAllocation; }
OSMetaClass IOService::gMetaClass("IOService",nullptr,sizeof(IOService));
static OSMetaClass apple("AppleARMBacklight",&IOService::gMetaClass,sizeof(IOService));
const OSSymbol *OSSymbol::withCString(const char *s) {
    if(failAlloc())return nullptr;auto *v=new OSSymbol;v->name=s;return v;
}
const OSMetaClass *OSMetaClass::getMetaClassWithName(const OSSymbol *s) {
    return applePresent && !strcmp(s->name,"AppleARMBacklight")?&apple:nullptr;
}
OSNumber *OSNumber::withNumber(uint64_t v,unsigned bits) {
    if(failAlloc())return nullptr;auto *n=new OSNumber;n->value=v;n->bits=bits;return n;
}
OSSerialize *OSSerialize::binaryWithCapacity(unsigned n, void *editor, void *reference) {
    assert(n==32 && !editor && !reference);return failAlloc()?nullptr:new OSSerialize;
}
bool OSNumber::serialize(OSSerialize *s) const {
    s->words[0]=0xd3;s->words[1]=0x84000000U|bits;
    s->words[2]=uint32_t(value);s->words[3]=uint32_t(value>>32);return true;
}
OSDictionary *OSDictionary::withCapacity(unsigned) { return failAlloc()?nullptr:new OSDictionary; }
OSDictionary::~OSDictionary() { for(unsigned i=0;i<count;++i)values[i]->release(); }
bool OSDictionary::setObject(const char *s,OSObject *v) {
    if(++propertyCalls==failProperty)return false;
    for(unsigned i=0;i<count;++i)if(!strcmp(s,keys[i])) {
        v->retain();values[i]->release();values[i]=v;return true;
    }
    assert(count<16);v->retain();keys[count]=s;values[count++]=v;return true;
}
OSObject *OSDictionary::getObject(const char *s) {
    for(unsigned i=0;i<count;++i)if(!strcmp(s,keys[i]))return values[i];return nullptr;
}
bool IOService::setProperty(const char *s,OSObject *v) { return properties.setObject(s,v); }
bool IOService::setProperty(const char *s,bool v) { return setProperty(s,uint64_t(v),32); }
bool IOService::setProperty(const char *s,uint64_t v,unsigned bits) {
    auto *n=OSNumber::withNumber(v,bits);if(!n)return false;
    bool ok=setProperty(s,n);n->release();return ok;
}
IOLock *IOLockAlloc() { if(failAlloc())return nullptr;++locks;return new IOLock; }
void IOLockFree(IOLock *l) { assert(!l->locked);--locks;delete l; }
void IOLockLock(IOLock *l) { assert(!l->locked);l->locked=true; }
void IOLockUnlock(IOLock *l) { assert(l->locked);l->locked=false; }
IOTimerEventSource *IOTimerEventSource::timerEventSource(OSObject *o,Action a) {
    if(failAlloc())return nullptr;assert(timerCount<2);auto *t=new IOTimerEventSource;
    t->owner=o;t->action=a;timers[timerCount++]=t;return t;
}
IOReturn IOTimerEventSource::setTimeoutMS(unsigned ms) {
    if(timerFailure)return kIOReturnIOError;armed=true;due=clockMs+ms;return 0;
}
IOReturn IOTimerEventSource::setTimeoutUS(unsigned) { return setTimeoutMS(0); }
IOWorkLoop *IOWorkLoop::workLoop() { return failAlloc()?nullptr:new IOWorkLoop; }
IOReturn IOWorkLoop::addEventSource(IOTimerEventSource *t) {
    if(++adds==failAdd)return kIOReturnIOError;t->retain();t->added=true;return 0;
}
void IOWorkLoop::removeEventSource(IOTimerEventSource *t) { assert(t->added);t->added=false;t->release(); }
uint64_t mach_absolute_time() { return clockMs*1000000; }
void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n) { *n=t; }
void IOLog(const char *,...) {}

// Execute the actual exported backend functions with a register model in place
// of MMIO. Locks and on/off/brightness arbitration are production code.
struct TestBus {
    uint8_t regs[2][256]={};unsigned operations=0,writes=0,fail=0;
    TestBus() {
        regs[0][0x46]=0x80;regs[1][0x46]=0x70;regs[1][0x50]=0x80;
        regs[1][0x53]=0xff;regs[1][0x54]=3;
    }
    int read(bool s,uint8_t r,uint8_t &v) {if(++operations==fail)return -1;v=regs[s][r];return 0;}
    int write(bool s,uint8_t r,uint8_t v) {if(++operations==fail)return -1;++writes;regs[s][r]=v;return 0;}
    void soft_start() {}
};
struct Backend:TestBus {
    picasso_backlight::State state;IOLock *lock=nullptr;IOService *display=nullptr;
    bool displayOn=true;uint32_t requested=1023;unsigned reports=0;
};
static Backend *backend;
#include "brightness-backend.inc"
#include "brightness-service.inc"

static OSDictionary *request(uint64_t brightness,uint64_t period=0) {
    auto *d=OSDictionary::withCapacity(2);auto *n=OSNumber::withNumber(brightness,64);
    d->setObject("brightness",n);n->release();n=OSNumber::withNumber(period,64);
    d->setObject("FadePeriod",n);n->release();return d;
}
static IOReturn send(IOService *s,uint64_t b,uint64_t p=0) {
    auto *d=request(b,p);auto r=s->setProperties(d);d->release();return r;
}
static void fire(unsigned i) {
    auto *t=timers[i];assert(t->armed && t->added);t->armed=false;t->action(t->owner,t);
}
static uint32_t published(IOService *s) {
    auto *d=dynamic_cast<OSDictionary *>(s->getProperty("IODisplayParameters"));assert(d);
    auto *b=dynamic_cast<OSDictionary *>(d->getObject("brightness"));assert(b);
    auto *n=dynamic_cast<OSNumber *>(b->getObject("value"));assert(n);return uint32_t(n->value);
}
static void reset() {
    clockMs=0;allocations=failAllocation=adds=failAdd=propertyCalls=failProperty=0;
    applePresent=true;timerFailure=false;timerCount=0;
}
int main() {
    IOService display,other;Backend b;backend=&b;b.display=&display;b.lock=IOLockAlloc();
    assert(!b.state.adopt(b));reset();unsigned cases=0;
    auto *s=picasso_brightness_allocate();assert(s->start(&display));
    unsigned startAllocations=allocations,startProperties=propertyCalls;
    assert(picasso_brightness_is_service(s) && !picasso_brightness_is_service(&other) && !picasso_brightness_is_service(nullptr));++cases;
    assert(s->getMetaClass()==&apple && s->getProperty("backlight-control") && published(s)==1023);++cases;
    assert(picasso_backlight_power(&other,false)==kIOReturnNotFound);++cases;
    unsigned ops=b.operations;
    assert(!send(s,2048) && !send(s,4095));assert(b.operations==ops && published(s)==1023);
    fire(0);assert(b.state.level==4095 && published(s)==4095);++cases;
    assert(!send(s,1));fire(0);assert(b.state.level==42 && published(s)==42);++cases;
    assert(!send(s,4095,65536));fire(0);assert(timers[1]->armed);
    clockMs=500;fire(1);assert(b.state.level==2068);++cases;
    assert(!send(s,1023));fire(0);assert(!timers[1]->armed && b.state.level==1023);++cases;
    assert(!picasso_backlight_power(&display,false));ops=b.operations;
    assert(!send(s,3000));fire(0);assert(!b.state.on && b.requested==3000 && b.operations==ops);++cases;
    assert(!picasso_backlight_power(&display,true));assert(b.state.on && b.state.level==3000);++cases;
    assert(!send(s,0));fire(0);assert(!b.state.on && !b.requested && !published(s));++cases;
    assert(!picasso_backlight_power(&display,true) && !b.state.on);++cases;
    assert(!send(s,2048));fire(0);assert(b.state.on && b.state.level==2048);++cases;
    assert(send(s,4096)==kIOReturnBadArgument && send(s,UINT64_MAX)==kIOReturnBadArgument);
    assert(send(s,5,UINT64_MAX)==kIOReturnBadArgument && s->setProperties(&display)==kIOReturnBadArgument);++cases;
    // The actual authorized 20H392 nested protocol. The API model stores the
    // same binary64 bits as OSNumber::withFloat/withDouble; never integer-casts.
    auto *outer=OSDictionary::withCapacity(1), *nested=OSDictionary::withCapacity(4);
    auto *normalized=OSNumber::withNumber(0,64);
    outer->setObject("DisplayBrightness",nested);nested->setObject("Brightness",normalized);
    const unsigned tags[]={31U,63U};
    for(unsigned tag: tags) {
        normalized->bits=tag;
        for(unsigned i=0;i<=4095;++i) {
            double f=double(i)/4095;memcpy(&normalized->value,&f,8);
            assert(!s->setProperties(outer));fire(0);
            assert(published(s)==picasso_backlight::clamp(i));
        }
        const uint64_t invalids[]={0xbfe0000000000000ULL,0x3ff0000000000001ULL,
                0x7ff0000000000000ULL,0x7ff8000000000000ULL,0xfff0000000000000ULL};
        for(uint64_t invalid: invalids) {
            normalized->value=invalid;assert(s->setProperties(outer)==kIOReturnBadArgument);
        }
        const uint64_t zeros[]={0ULL,0x8000000000000000ULL,1ULL};
        for(uint64_t zero: zeros) {
            normalized->value=zero;assert(!s->setProperties(outer));fire(0);assert(!published(s));
        }
    }
    normalized->bits=32;normalized->value=1;
    assert(!s->setProperties(outer));fire(0);assert(published(s)==4095);
    normalized->value=2;assert(s->setProperties(outer)==kIOReturnBadArgument);
    normalized->bits=63;normalized->value=0x3fe0000000000000ULL;
    failAllocation=allocations+1;assert(s->setProperties(outer)==kIOReturnNoMemory);failAllocation=0;
    assert(!picasso_backlight_power(&display,false));ops=b.operations;
    assert(!s->setProperties(outer));fire(0);assert(b.operations==ops && b.requested==2048);
    assert(!picasso_backlight_power(&display,true) && b.state.level==2048);
    outer->setObject("brightness",normalized);assert(s->setProperties(outer)==kIOReturnBadArgument);
    normalized->release();nested->release();outer->release();cases+=8208;
    assert(!send(s,1000,0xffff0000));fire(0);assert(published(s)==1000 && !timers[1]->armed);
    assert(!send(s,2048));fire(0);
    timerFailure=true;assert(send(s,3000)==kIOReturnIOError);timerFailure=false;++cases;
    assert(!send(s,3000));b.fail=b.operations+1;fire(0);
    assert(b.state.failed && published(s)==2048 && s->getProperty("PicassoBrightnessError"));
    assert(send(s,4095)==kIOReturnNotReady);++cases;
    s->stop(&display);assert(!picasso_brightness_is_service(s));assert(send(s,10)==kIOReturnNotReady);s->release();assert(locks==1);++cases;
    b.fail=0;b.state.failed=false;
    for(unsigned i=1;i<=startAllocations;++i) {
        reset();failAllocation=i;s=picasso_brightness_allocate();assert(!s->start(&display));
        s->stop(&display);s->release();assert(locks==1);++cases;
    }
    for(unsigned i=1;i<=startProperties;++i) {
        reset();failProperty=i;s=picasso_brightness_allocate();assert(!s->start(&display));
        s->stop(&display);s->release();assert(locks==1);++cases;
    }
    for(unsigned i=1;i<=2;++i) {
        reset();failAdd=i;s=picasso_brightness_allocate();assert(!s->start(&display));
        s->stop(&display);s->release();assert(locks==1);++cases;
    }
    reset();applePresent=false;s=picasso_brightness_allocate();assert(!s->start(&display));s->release();++cases;
    reset();s=picasso_brightness_allocate();assert(s->start(&display));
    auto *bad=OSDictionary::withCapacity(2);auto *num=OSNumber::withNumber(1023,32);
    bad->setObject("brightness",num);num->release();bad->setObject("FadePeriod",&display);
    assert(s->setProperties(bad)==kIOReturnBadArgument);bad->release();++cases;
    assert(!send(s,42,65536));fire(0);clockMs=1000;fire(1);
    assert(published(s)==42 && b.state.level==42 && !timers[1]->armed);++cases;
    assert(!send(s,3000));ops=b.operations;s->stop(&display);
    assert(!timers[0]->armed && !timers[1]->armed && b.operations==ops);s->release();++cases;
    reset();s=picasso_brightness_allocate();assert(s->start(&display));assert(!send(s,1023));
    failAllocation=allocations+1;fire(0);assert(send(s,5)==kIOReturnNotReady);
    s->stop(&display);s->release();++cases;
    reset();b.state.failed=true;s=picasso_brightness_allocate();assert(!s->start(&display));
    assert(!s->getProperty("backlight-control"));s->release();++cases;
    IOLockFree(b.lock);assert(!locks);
    printf("PASS brightness: %u production service/backend, mailbox, fade, power and failure cases\n",cases);
}
