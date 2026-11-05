// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production callback/consumer bodies with deterministic IOKit stand-ins.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct OSObject {int refs=1;void retain(){++refs;}void release(){assert(refs>0);--refs;}};
struct IOService:OSObject {};
struct IOLock {bool locked=false;};
static IOLock storage;
static bool allocFails;
static IOLock *IOLockAlloc(){return allocFails?nullptr:&storage;}
static void IOLockLock(IOLock *l){assert(!l->locked);l->locked=true;}
static void IOLockUnlock(IOLock *l){assert(l->locked);l->locked=false;}
#define OSSafeReleaseNULL(p) do{if(p)(p)->release();(p)=nullptr;}while(0)
#define OSDynamicCast(t,p) static_cast<t *>(p)
static void IOLog(const char *,...){}
#include "button-sink.inc"
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoMemory=2,kIOReturnBadArgument=3,kIODirectionNone=0};
struct IOBufferMemoryDescriptor:OSObject {
    uint8_t report[2];
    static bool fail;
    static IOBufferMemoryDescriptor buffer;
    static IOBufferMemoryDescriptor *withBytes(const void *p,unsigned size,int) {
        assert(size==2);if(fail)return nullptr;
        buffer.refs=1;buffer.report[0]=((const uint8_t *)p)[0];buffer.report[1]=((const uint8_t *)p)[1];return &buffer;
    }
};
bool IOBufferMemoryDescriptor::fail;
IOBufferMemoryDescriptor IOBufferMemoryDescriptor::buffer;
struct IOWorkLoop {
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    bool stopBeforeDispatch=false;
    Action before=nullptr;
    IOReturn runAction(Action a,OSObject *o,void *arg=nullptr) {
        if(stopBeforeDispatch){stopBeforeDispatch=false;before(o,nullptr,nullptr,nullptr,nullptr);}
        return a(o,arg,nullptr,nullptr,nullptr);
    }
};
static void native_hid_input_observed(unsigned,int){}
struct NativeHidInput:IOService {
    IOWorkLoop loop;IOWorkLoop *inputLoop=&loop;
    bool buttonsStopping=false,down=false,ready=true;
    uint8_t buttons=0,last=0;unsigned submitted=0;
    IOReturn result=0;
    bool hasEventDriverCached(){return ready;}
    IOReturn submitReport(IOBufferMemoryDescriptor *b){assert(b->report[0]==2);++submitted;last=b->report[1];return result;}
#include "button-consumer.inc"
};
static bool unregisterDuringCallback(void *c,uint32_t) {
    auto *p=static_cast<IOService *>(c);assert(p->refs==3);
    native_buttons_set_sink(nullptr,nullptr);assert(p->refs==2);return true;
}
int main() {
    NativeHidInput d;
    allocFails=true;native_buttons_set_sink(NativeHidInput::buttonsNotify,&d);
    assert(!picasso_buttons_available() && !picasso_buttons_deliver(2) && d.refs==1);
    allocFails=false;native_buttons_set_sink(NativeHidInput::buttonsNotify,&d);
    assert(picasso_buttons_available() && d.refs==2);
    d.ready=false;assert(!picasso_buttons_deliver(2) && !d.buttons);
    d.ready=true;IOBufferMemoryDescriptor::fail=true;assert(!picasso_buttons_deliver(2));
    IOBufferMemoryDescriptor::fail=false;d.result=5;assert(!picasso_buttons_deliver(2) && !d.buttons);
    d.result=0;assert(picasso_buttons_deliver(2) && d.buttons==2 && d.last==2);
    d.down=true;assert(picasso_buttons_deliver(14) && d.last==15);
    assert(picasso_buttons_deliver(0) && d.last==1 && d.buttons==0); // preserve automatic Home
    d.down=false;assert(picasso_buttons_deliver(8));
    d.loop.stopBeforeDispatch=true;d.loop.before=NativeHidInput::stopButtons;
    assert(!picasso_buttons_deliver(4) && d.buttonsStopping && d.last==0 && d.buttons==0);
    native_buttons_set_sink(nullptr,nullptr);assert(!picasso_buttons_available() && d.refs==1);
    assert(!picasso_buttons_deliver(2));
    native_buttons_set_sink(unregisterDuringCallback,&d);
    assert(picasso_buttons_deliver(2) && d.refs==1 && !picasso_buttons_available());
    NativeHidInput failed;
    native_buttons_set_sink(NativeHidInput::buttonsNotify,&failed);
    assert(picasso_buttons_deliver(2));failed.result=5;
    assert(!picasso_buttons_deliver(0x80000000u));
    assert(failed.buttonsStopping && failed.buttons==0 && failed.last==0);
    assert(!picasso_buttons_deliver(4));
    native_buttons_set_sink(nullptr,nullptr);
    puts("PASS: real sink retention, unregister during callback, report failures, Home preservation and stop-before-dispatch rejection");
}
