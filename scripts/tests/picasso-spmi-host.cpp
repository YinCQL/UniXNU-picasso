// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "PicassoSpmiCore.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using Mutex=CRITICAL_SECTION;
static void init(Mutex *m){InitializeCriticalSection(m);}
static void enter(Mutex *m){EnterCriticalSection(m);}
static void leave(Mutex *m){LeaveCriticalSection(m);}
static void destroy(Mutex *m){DeleteCriticalSection(m);}
static void yield(){SwitchToThread();}
#else
#include <pthread.h>
#include <sched.h>
using Mutex=pthread_mutex_t;
static void init(Mutex *m){assert(!pthread_mutex_init(m,nullptr));}
static void enter(Mutex *m){assert(!pthread_mutex_lock(m));}
static void leave(Mutex *m){assert(!pthread_mutex_unlock(m));}
static void destroy(Mutex *m){assert(!pthread_mutex_destroy(m));}
static void yield(){sched_yield();}
#endif
struct Controller {
    Mutex mutex;
    unsigned locks=0,unlocks=0,active=0,owner=0,commands=0,dataReads=0;
    Controller(){init(&mutex);}
    ~Controller(){destroy(&mutex);}
};
struct Bus {
    Controller &c;unsigned id;
    uint32_t statuses[3]={1,1,1},cmd=0;
    unsigned calls=0,polls=0,delays=0,doneAfter=0,data=0xab;
    bool writes=false;
    bool *stopOnUnlock=nullptr;
    int lock(){enter(&c.mutex);assert(!c.active);c.active=1;c.owner=id;++c.locks;return 17;}
    void unlock(int state){assert(state==17 && c.active==1 && c.owner==id);c.active=0;++c.unlocks;if(stopOnUnlock)__atomic_store_n(stopOnUnlock,true,__ATOMIC_RELEASE);leave(&c.mutex);}
    void write(unsigned off,uint32_t value){
        assert(c.active==1 && c.owner==id);
        if(off==0x10){assert(writes && value==0x56);return;}
        assert(off==0);cmd=value;polls=0;++calls;++c.commands;
        // Force another APID's thread to run before DONE and data retrieval.
        yield();assert(c.owner==id);
    }
    uint32_t read(unsigned off){
        assert(c.active==1 && c.owner==id);
        if(off==8)return polls++>=doneAfter?statuses[calls>3?2:calls-1]:0;
        assert(off==0x18 && !writes);++c.dataReads;return data;
    }
    void delay(){++delays;}
};
struct Work {Controller *controller;unsigned id;};
#ifdef _WIN32
static DWORD WINAPI worker(void *opaque)
#else
static void *worker(void *opaque)
#endif
{
    auto *w=static_cast<Work *>(opaque);Bus b{*w->controller,w->id};b.writes=w->id==2;
    for(unsigned i=0;i<4000;++i){
        if(b.writes)assert(!picasso_spmi::write8(b,0x53,0x56) && b.cmd==0x530);
        else {uint8_t value=0;unsigned retries=9;
            assert(!picasso_spmi::read8(b,0x10,value,retries) && value==0xab && retries==0 && b.cmd==0x08000100);}
    }
    return 0;
}
int main(){
    unsigned cases=0;
    for(unsigned status=1;status<16;status+=2){
        Controller c;Bus b{c,1};b.statuses[0]=b.statuses[1]=b.statuses[2]=status;b.doneAfter=3;
        uint8_t value=0x77;unsigned retries=99;
        int error=picasso_spmi::read8(b,0x10,value,retries);
        unsigned attempts=status==9?3:1;
        assert(error==int(status&14) && b.calls==attempts && retries==attempts-1);
        assert(c.locks==attempts && c.unlocks==attempts && !c.active);
        assert(error?(value==0x77 && !c.dataReads):(value==0xab && c.dataReads==1));++cases;
    }
    for(unsigned recovery=1;recovery<=2;++recovery){
        Controller c;Bus b{c,1};for(unsigned i=0;i<recovery;++i)b.statuses[i]=9;
        uint8_t value=0;unsigned retries=0;
        assert(!picasso_spmi::read8(b,0x54,value,retries) && retries==recovery && value==0xab);
        assert(c.commands==recovery+1 && c.dataReads==1 && !c.active);++cases;
    }
    {
        Controller c;Bus b{c,1};b.statuses[0]=0;uint8_t value=0x77;unsigned retries=99;
        assert(picasso_spmi::read8(b,0x46,value,retries)==-1 && value==0x77 && !retries);
        assert(c.commands==1 && !c.dataReads && b.delays==1000 && c.unlocks==1 && !c.active);++cases;
    }
    for(unsigned status=0;status<16;++status){
        Controller c;Bus b{c,1};b.writes=true;b.statuses[0]=status;
        int error=picasso_spmi::write8(b,0x53,0x56);
        assert(error==(status&1?int(status&14):-1)); // A write is never retried.
        assert(c.commands==1 && !c.dataReads && c.unlocks==1 && !c.active);++cases;
    }
    {
        Controller c;Bus b{c,1};bool stopped=true;uint8_t value=0x77;unsigned retries=99;
        assert(picasso_spmi::read8(b,0x10,value,retries,&stopped)==-4 && value==0x77);
        b.writes=true;assert(picasso_spmi::write8(b,0x53,0x56,&stopped)==-4);
        assert(!c.commands&&!c.dataReads&&c.locks==2&&c.unlocks==2&&!c.active);
    }
    {
        Controller c;Bus b{c,1};bool stopped=false;b.stopOnUnlock=&stopped;b.statuses[0]=9;
        uint8_t value=0x77;unsigned retries=0;
        assert(picasso_spmi::read8(b,0x10,value,retries,&stopped)==-4 && value==0x77);
        assert(stopped&&retries==1&&c.commands==1&&!c.dataReads&&c.locks==2&&c.unlocks==2);
    }
    // Four APIDs (buttons, WLED observer/writer) contend on one controller.
    Controller c;Work w[4]={{&c,1},{&c,2},{&c,3},{&c,4}};
#ifdef _WIN32
    HANDLE threads[4];
    for(unsigned i=0;i<4;++i){threads[i]=CreateThread(nullptr,0,worker,&w[i],0,nullptr);assert(threads[i]);}
    assert(WaitForMultipleObjects(4,threads,TRUE,INFINITE)==WAIT_OBJECT_0);
    for(auto t:threads)CloseHandle(t);
#else
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],nullptr,worker,&w[i]));
    for(auto t:threads)assert(!pthread_join(t,nullptr));
#endif
    assert(c.commands==16000 && c.locks==16000 && c.unlocks==16000 && c.dataReads==12000 && !c.active);
    printf("PASS SPMI: %u status/timeout/retry cases and 16000 concurrent APID transactions\n",cases);
}
